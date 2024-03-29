#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h> 
#include <errno.h>
#include <semaphore.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "common/lock.h"
#include "common/message_queue.h"
#include "socket/socket_pipe.h"
#include "socket/socket_util.h"

struct startup_args {
	int fd;
	char* args;
	sem_t sem;
};

typedef struct workder_ctx {
	mutex_t mutex;
	cond_t cond;
	struct message_queue* queue;
	int id;
	int quit;
	int ref;
	int callback;

	lua_State* L;

	int fd;
	struct pipe_message* first;
	struct pipe_message* last;
} worker_ctx_t;

typedef struct worker_manager {
	mutex_t mutex;
	int size;
	struct workder_ctx** slot;
} worker_manager_t;


static worker_manager_t* _MANAGER = NULL;
static pthread_once_t _MANAGER_INIT = PTHREAD_ONCE_INIT;

void
create_manager() {
	worker_manager_t* wm = malloc(sizeof(*wm));
	memset(wm,0,sizeof(*wm));
	wm->size = 8;
	wm->slot = malloc(sizeof(*wm->slot) * wm->size);
	memset(wm->slot,0,sizeof(*wm->slot) * wm->size);
	mutex_init(&wm->mutex);

	_MANAGER = wm;
}

void
add_worker(worker_ctx_t* ctx) {
	mutex_lock(&_MANAGER->mutex);
	int i;
	for(i = 0;i < _MANAGER->size;i++) {
		worker_ctx_t* node = _MANAGER->slot[i];
		if (node == NULL) {
			_MANAGER->slot[i] = ctx;
			ctx->id = i;
			mutex_unlock(&_MANAGER->mutex);
			return;
		}
	}

	int nsize = _MANAGER->size * 2;
	worker_ctx_t** nslot = malloc(sizeof(*nslot) * nsize);
	memset(nslot,0,sizeof(*nslot) * nsize);
	for(i=0;i<_MANAGER->size;i++) {
		worker_ctx_t* node = _MANAGER->slot[i];
		assert(node != NULL);
		nslot[i] = node;
	}

	nslot[_MANAGER->size] = ctx;
	ctx->id = _MANAGER->size;
	
	free(_MANAGER->slot);
	_MANAGER->slot = nslot;
	_MANAGER->size = nsize;
	mutex_unlock(&_MANAGER->mutex);
}

void
delete_worker(int id) {
	mutex_lock(&_MANAGER->mutex);
	_MANAGER->slot[id] = NULL;
	mutex_unlock(&_MANAGER->mutex);
}

worker_ctx_t*
worker_ref(int id) {
	mutex_lock(&_MANAGER->mutex);
	worker_ctx_t* ctx = _MANAGER->slot[id];
	if (ctx) {
		__sync_add_and_fetch(&ctx->ref,1);
	}
	mutex_unlock(&_MANAGER->mutex);
	return ctx;
}

void worker_release(worker_ctx_t* ctx);

void
worker_unref(worker_ctx_t* ctx) {
	int ref = __sync_sub_and_fetch(&ctx->ref,1);
	if (ref == 0) {
		worker_release(ctx);
	}
}

worker_ctx_t*
worker_create() {
	worker_ctx_t* worker_ctx = malloc(sizeof(*worker_ctx));
	memset(worker_ctx,0,sizeof(*worker_ctx));
	
	mutex_init(&worker_ctx->mutex);
	cond_init(&worker_ctx->cond);

	worker_ctx->id = -1;
	worker_ctx->quit = 0;
	worker_ctx->ref = 1;
	worker_ctx->queue = queue_create();

	add_worker(worker_ctx);
	return worker_ctx;
}

void
workder_quit(worker_ctx_t* ctx) {
	delete_worker(ctx->id);
	worker_unref(ctx);
}

void
worker_release(worker_ctx_t* ctx) {
	queue_free(ctx->queue);
	mutex_destroy(&ctx->mutex);
	cond_destroy(&ctx->cond);
	while(ctx->first) {
		struct pipe_message* message = ctx->first;
		ctx->first = ctx->first->next;
		if (message->data)
			free(message->data);
		free(message);
	}
	lua_close(ctx->L);
}

int
worker_send_pipe(worker_ctx_t* ctx) {
	while(ctx->first) {
		struct pipe_message* message = ctx->first;
		struct pipe_message* next_message = message->next;
		if (socket_pipe_write(ctx->fd, (void*)&message, sizeof(void*)) < 0) {
			return -1;
		}
		ctx->first = next_message;
	}
	ctx->first = ctx->last = NULL;
	return 0;
}

int
worker_push(int target,int source,int session,void* data,size_t size) {
	worker_ctx_t* target_ctx = worker_ref(target);
	if (!target_ctx) {
		return -1;
	}

	queue_push(target_ctx->queue,source,session,data,size);
	if (!mutex_trylock(&target_ctx->mutex)) {
		cond_notify_one(&target_ctx->cond);
		mutex_unlock(&target_ctx->mutex);
	}

	worker_unref(target_ctx);
	return 0;
}

void
worker_callback(worker_ctx_t* ctx,int source,int session,void* data,int size) {
	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback);

	lua_pushinteger(ctx->L,source);
	lua_pushinteger(ctx->L,session);
	if (data) {
		lua_pushlightuserdata(ctx->L,data);
		lua_pushinteger(ctx->L,size);
		lua_pcall(ctx->L,4,0,0);
		free(data);
	} else {
		lua_pcall(ctx->L,2,0,0);
	}
}

void
worker_dispatch(worker_ctx_t* ctx) {
	struct message_queue* queue_ctx = ctx->queue;
	for(;;) {
		struct queue_message* message = queue_pop(queue_ctx,ctx->id);
		if (message == NULL) {
			mutex_lock(&ctx->mutex);
			
			cond_timed_wait(&ctx->cond,&ctx->mutex,10);

			mutex_unlock(&ctx->mutex);

			worker_send_pipe(ctx);
		} else {
			worker_callback(ctx,message->source,message->session,message->data,message->size);
			worker_send_pipe(ctx);
			if (ctx->quit) {
				workder_quit(ctx);
				break;
			}
		}
	}
}



extern int load_helper(lua_State *L);

int
module_push(lua_State* L) {
	worker_ctx_t* ctx = lua_touserdata(L, 1);
	int target = lua_tointeger(L,2);
	int session = lua_tointeger(L,3);

	void* data = NULL;
	size_t size = 0;

	switch(lua_type(L,4)) {
		case LUA_TSTRING: {
			const char* str = lua_tolstring(L,4,&size);
			data = malloc(size);
			memcpy(data,str,size);
			break;
		}
		case LUA_TUSERDATA:{
			data = lua_touserdata(L,4);
			size = lua_tointeger(L,5);
			break;
		}
		default:
			luaL_error(L,"unkown type:%s",lua_typename(L,lua_type(L,4)));
	}

	if (worker_push(target,ctx->id,session,data,size) < 0) {
		lua_pushboolean(L,0);
		return 1;
	}
	lua_pushboolean(L,1);
	return 1;
}

int
send_pipe(lua_State* L) {
	worker_ctx_t* ctx = lua_touserdata(L, 1);
	int session = lua_tointeger(L,2);

	void* data = NULL;
	size_t size = 0;

	switch(lua_type(L,3)) {
		case LUA_TSTRING: {
			const char* str = lua_tolstring(L, 3, &size);
			data = malloc(size);
			memcpy(data,str,size);
			break;
		}
		case LUA_TLIGHTUSERDATA:{
			data = lua_touserdata(L, 3);
			size = lua_tointeger(L, 4);
			break;
		}
		default:
			luaL_error(L,"unkown type:%s",lua_typename(L,lua_type(L,3)));
	}

	struct pipe_message* message = malloc(sizeof(*message));
	message->next = NULL;
	message->source = ctx->id;
	message->session = session;
	message->data = data;
	message->size = size;
	if (ctx->first == NULL) {
		ctx->first = ctx->last = message;
	} else {
		ctx->last->next = message;
		ctx->last = message;
	}
	return 0;
}

int 
quit(lua_State* L) {
	worker_ctx_t* ctx = lua_touserdata(L, 1);
	ctx->quit = 1;
	return 0;
}

int 
dispatch(lua_State* L) {
	worker_ctx_t* ctx = lua_touserdata(L, 1);
	luaL_checktype(L,2,LUA_TFUNCTION);
	ctx->callback = luaL_ref(L,LUA_REGISTRYINDEX);
	return 0;
}

void*
_worker(void* ud) {
	struct startup_args* args = ud;
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	luaL_requiref(L,"helper",load_helper,0);

	luaL_newmetatable(L, "meta_worker");
	const luaL_Reg meta_worker[] = {
		{ "push", module_push },
		{ "send_pipe", send_pipe },
		{ "quit", quit },
		{ "dispatch", dispatch },
		{ NULL, NULL },
	};
	luaL_newlib(L,meta_worker);
	lua_setfield(L, -2, "__index");
	lua_pop(L,1);

	lua_settop(L,0);

	if (luaL_loadfile(L,"lualib/bootstrap.lua") != LUA_OK)  {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		exit(1);
	}

	lua_pushinteger(L, 2);
	int argc = 1;
	char *token;
	char* boot = args->args;
	for(token = strsep(&boot, "@"); token != NULL; token = strsep(&boot, "@")) {
		lua_pushstring(L, token);
		argc++;
	}

	worker_ctx_t* ctx = lua_newuserdata(L,sizeof(*ctx));
	memset(ctx,0,sizeof(*ctx));
	
	mutex_init(&ctx->mutex);
	cond_init(&ctx->cond);

	ctx->id = -1;
	ctx->quit = 0;
	ctx->ref = 1;
	ctx->queue = queue_create();
	ctx->fd = args->fd;

	luaL_newmetatable(L,"meta_worker");
 	lua_setmetatable(L, -2);

	add_worker(ctx);

	++argc;

	if (lua_pcall(L,argc,0,0) != LUA_OK)  {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		exit(1);
	}
	ctx->L = L;
	sem_post(&args->sem);
	worker_dispatch(ctx);
	sem_destroy(&args->sem);
	free(args->args);
	free(args);
	return NULL;
}


int
create(lua_State* L) {
	pthread_once(&_MANAGER_INIT,&create_manager);

	int fd = lua_tointeger(L, 1);
	const char* startup_args = lua_tostring(L, 2);

	struct startup_args* args = malloc(sizeof(*args));
	args->fd = fd;
	args->args = strdup(startup_args);
	sem_init(&args->sem, 0, -1);

	pthread_t pid;
	if (pthread_create(&pid, NULL, _worker, args)) {
		exit(1);
	}
	sem_wait(&args->sem);
	lua_pushinteger(L, pid);
	return 1;
}

int
join(lua_State* L) {
	pthread_t pid = lua_tointeger(L, 1);
	pthread_join(pid, NULL);
	return 0;
}

int
main_push(lua_State* L) {
	int target = lua_tointeger(L, 1);
	int session = lua_tointeger(L, 2);

	void* data = NULL;
	size_t size = 0;

	switch(lua_type(L,3)) {
		case LUA_TSTRING: {
			const char* str = lua_tolstring(L, 3, &size);
			data = malloc(size);
			memcpy(data,str,size);
			break;
		}
		case LUA_TLIGHTUSERDATA:{
			data = lua_touserdata(L, 3);
			size = lua_tointeger(L, 4);
			break;
		}
		default: {
			luaL_error(L,"unkown type:%s",lua_typename(L,lua_type(L,3)));
		}
	}

	if (worker_push(target,-1,session,data,size) < 0) {
		lua_pushboolean(L,0);
		return 1;
	}
	lua_pushboolean(L,1);
	return 1;
}

int
luaopen_worker_core(lua_State* L) {
	const luaL_Reg l[] = {
		{ "create", create },
		{ "join", join },
		{ "push", main_push },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}