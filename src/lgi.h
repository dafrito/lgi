/*
 * Dynamic Lua binding to GObject using dynamic gobject-introspection.
 *
 * Author: Pavel Holejsovsky (pavel.holejsovsky@gmail.com)
 *
 * License: MIT.
 */

#define G_LOG_DOMAIN "Lgi"

#include <lua.h>
#include <lauxlib.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gprintf.h>
#include <girepository.h>

/* Lua stack dump for debugging purposes. */
#ifndef NDEBUG
const char *lgi_sd (lua_State* L);
#endif

/* Makes sure that Lua stack offset is absolute one, not relative. */
#define lgi_makeabs(L, x) do { if (x < 0) x += lua_gettop (L) + 1; } while (0)

/* Puts parts of the name to the stack, to be concatenated by lua_concat.
   Returns number of pushed elements. */
int lgi_type_get_name (lua_State *L, GIBaseInfo *info);

/* Allocates guard, a pointer-size userdata with associated destroy
   handler. Returns stack index of the allocated userdata. */
int lgi_guard_create (lua_State *L, gpointer **data, GDestroyNotify destroy);

/* Returns data of specified guard. */
void lgi_guard_get_data (lua_State *L, int pos, gpointer **data);

/* Allocates guard which guards specified GIBaseInfo instance. */
int lgi_guard_create_baseinfo (lua_State *L, GIBaseInfo *info);

/* Key in registry, containing table with all our private data. */
extern int lgi_regkey;
typedef enum lgi_reg
{
  /* Cache of created userdata objects, __mode=v */
  LGI_REG_CACHE = 1,

  /* compound.ref_repo -> repo type table. */
  LGI_REG_TYPEINFO = 2,

  /* Whole repository, filled in by bootstrap. */
  LGI_REG_REPO = 3,

  /* GLib log_handler method. */
  LGI_REG_LOG_HANDLER = 4,

  LGI_REG__LAST
} LgiRegType;

/* Generic flags used in the interface. */
enum _LgiFlags
  {
    LGI_FLAGS_OPTIONAL =    1 << 0
  } LgiFlags;

/* Initialization of modules. */
void lgi_record_init (lua_State *L);
void lgi_compound_init (lua_State *L);
void lgi_callable_init (lua_State *L);

/* Gets gtype of the type represented by typeinfo. */
GType lgi_get_gtype (lua_State *L, GITypeInfo *ti);

/* Marshalls single value from Lua to GLib/C. Returns number of temporary
   entries pushed to Lua stack, which should be popped before function call
   returns. */
int lgi_marshal_arg_2c (lua_State *L, GITypeInfo *ti, GIArgInfo *ai,
			GITransfer xfer,  GIArgument *val, int narg,
			gboolean use_pointer, GICallableInfo *ci, void **args);

/* Marshalls single value from Lua to GValue. ti is optional. */
void lgi_marshal_val_2c (lua_State *L, GITypeInfo *ti, GITransfer xfer,
			 GValue *val, int narg);

/* If given parameter is out;caller-allocates, tries to perform
   special 2c marshalling.  If not needed, returns FALSE, otherwise
   stores single value with value prepared to be returned to C. */
gboolean lgi_marshal_arg_2c_caller_alloc (lua_State *L, GITypeInfo *ti,
					  GIArgument *val, int pos);

/* Marshalls single value from GLib/C to Lua. If parent is non-0, it
   is stack index of parent structure/array in which this C value
   resides. */
void lgi_marshal_arg_2lua (lua_State *L, GITypeInfo *ti, GITransfer xfer,
			   GIArgument *val, int parent, gboolean use_pointer,
			   GICallableInfo *ci, void **args);

/* Marshalls single value from GValue to Lua. ti is optional. */
void lgi_marshal_val_2lua (lua_State *L, GITypeInfo *ti, GITransfer xfer,
			   const GValue *val);

/* Parses given GICallableInfo, creates new userdata for it and stores
   it to the stack. Uses cache, so already parsed callable held in the
   cache is reused if possible. */
int lgi_callable_create (lua_State *L, GICallableInfo *ci);

/* Calls specified callable and arguments on the stack, using passed function
   address.  If it is NULL, an address is attempted to get from the info (if it
   is actually IFunctionInfo). func is stack index of callable object and args
   is stack index of first argument. */
int lgi_callable_call (lua_State *L, gpointer addr, int func, int args);

/* Creates closure for specified Lua function (or callable table or
   userdata). Returns user_data field for the closure and fills call_addr with
   executable address for the closure. */
gpointer lgi_closure_create (lua_State* L, GICallableInfo* ci, int target,
			     gboolean autodestroy, gpointer* call_addr);

/* GDestroyNotify-compatible callback for destroying closure. */
void lgi_closure_destroy (gpointer user_data);

/* Creates closure guard Lua userdata object and puts it on the stack.
   Closure guard automatically destroys the closure in its __gc
   metamethod. */
void lgi_closure_guard (lua_State *L, gpointer user_data);

/* Creates new compound of given address and type, pushes its userdata
   on the lua stack. Parent is 0 or stack index of parent item, which
   owns memory in which registered compound lives.  Returns 1 if
   successful, 0 otherwise. */
int lgi_compound_create (lua_State *L, GIBaseInfo *ii, gpointer addr,
			 gboolean own, int parent);

/* Creates new struct including allocated place for it. When
   owner_parent is not 0, it is stack position of the parent on which
   is the structure allocated, so it is kept by this element. */
gpointer lgi_compound_struct_new (lua_State *L, GIBaseInfo *ii);

/* Creates new object, initializes with specified properties. */
gpointer lgi_compound_object_new (lua_State *L, GIObjectInfo *ii, int argtable);

/* Retrieves compound-type parameter from given Lua-stack position,
   checks, whether it is suitable for requested gtype.  Fills in
   pointer to the compound object, or NULL if Lua-stack value is nil
   and optional is TRUE.  Returns number of temporary Lua objects
   pushed to the stack. On return, fills gtype argument with real
   gtype of returned compound. */
int lgi_compound_get (lua_State *L, int arg, GType *gtype, gpointer *addr,
		      unsigned flags);

/* Checks, compound with reqeusted gtype lives at given stack position.  If
   yes, returns its address and updates real compound's gtype, otherwise
   returns NULL.  Does not do any conversions/errors. */
gpointer lgi_compound_check (lua_State *L, int arg, GType *gtype);

/* Creates GClosure which invokes specified target. */
GClosure *lgi_gclosure_create (lua_State *L, int target);

/* Record ownership modes. */
typedef enum
  {
    LGI_RECORD_PEEK,
    LGI_RECORD_PARENT,
    LGI_RECORD_OWN,
    LGI_RECORD_ALLOCATE,
  } LgiRecordMode;

/* Creates Lua-side part of given record. Pushes the object
   representing it on the stack. In 'allocate' mode, new instance of
   the record is allocated and managed in Lua heap. If parent not
   zero, it is stack index of record parent (i.e. reocrd of which the
   arg record is part of). */
gpointer lgi_record_2lua (lua_State *L, GIBaseInfo *ri, gpointer addr,
			  LgiRecordMode mode, int parent);

/* Gets pointer to C-structure from given Lua-side object. Returns
   number of temporary objects created pushed on the stack. */
int lgi_record_2c (lua_State *L, GIBaseInfo *ri, int narg, gpointer *addr,
		   gboolean optional);
