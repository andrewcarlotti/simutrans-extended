#ifndef _API_OBJ_DESC_BASE_H_
#define _API_OBJ_DESC_BASE_H_

/** @file api_obj_besch.h templates for transfer of desc-pointers */

#include "../api_param.h"
#include "../api_class.h"

class obj_desc_std_name_t;
class obj_desc_timelined_t;
class obj_desc_transport_related_t;
class baum_desc_t;
class haus_desc_t;
class haus_tile_desc_t;
class ware_desc_t;
class weg_desc_t;

namespace script_api {

#define declare_desc_param(T, sqtype) \
	template<> \
	struct param<const T*> { \
		\
		typedef const T* (*GETFUNC)(const char*);\
		\
		static GETFUNC getfunc(); \
		\
		declare_types("t|x|y", sqtype); \
		\
		static void* tag() \
		{ \
			return (void*)(getfunc()); \
		} \
		static const T* get(HSQUIRRELVM vm, SQInteger index); \
		\
		static SQInteger push(HSQUIRRELVM vm, const T* b); \
	}; \
	template<> \
	struct param<T*> { \
		\
		declare_types("t|x|y", sqtype); \
	};

#define implement_desc_param(T, sqtype, func) \
	param<const T*>::GETFUNC param<const T*>::getfunc() \
	{ \
		return func; \
	} \
	const T* param<const T*>::get(HSQUIRRELVM vm, SQInteger index) \
	{ \
		return (const T*)get_instanceup(vm, index, tag(), sqtype); \
	} \
	SQInteger param<const T*>::push(HSQUIRRELVM vm, const T* b) \
	{ \
		if (b) { \
			return push_instance(vm, sqtype, b->get_name()); \
		} \
		else { \
			sq_pushnull(vm); return 1; \
		} \
	}

	declare_specialized_param(const obj_desc_std_name_t*, "t|x|y", "obj_desc_x");
	declare_param_mask(obj_desc_std_name_t*, "t|x|y", "obj_desc_x");

	declare_specialized_param(const obj_desc_timelined_t*, "t|x|y", "obj_desc_time_x");
	declare_param_mask(obj_desc_timelined_t*, "t|x|y", "obj_desc_time_x");

	declare_specialized_param(const obj_desc_transport_related_t*, "t|x|y", "obj_desc_transport_x");
	declare_param_mask(obj_desc_transport_related_t*, "t|x|y", "obj_desc_transport_x");

	declare_desc_param(baum_desc_t, "tree_desc_x");
	declare_desc_param(ware_desc_t, "good_desc_x");
	declare_desc_param(haus_desc_t, "building_desc_x");
	declare_desc_param(weg_desc_t, "way_desc_x");

	// only push the haus_desc_t-pointer
	declare_desc_param(haus_tile_desc_t, "building_desc_x");
};

#endif
