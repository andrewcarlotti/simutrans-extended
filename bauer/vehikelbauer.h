/*
 * Copyright (c) 1997 - 2002 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 * (see licence.txt)
 */

#ifndef vehikelbauer_t_h
#define vehikelbauer_t_h


#include "../dataobj/koord3d.h"
#include "../simtypes.h"
#include <string>

class vehicle_t;
class player_t;
class convoi_t;
class vehikel_desc_t;
class ware_desc_t;
template <class T> class slist_tpl;


/**
 * Baut Fahrzeuge. Fahrzeuge sollten nicht direct instanziiert werden
 * sondern immer von einem vehikelbauer_t erzeugt werden.
 *
 * Builds vehicles. Vehicles should not be instantiated directly,
 * but always from a vehikelbauer_t produced. (Google)
 *
 * @author Hj. Malthaner
 */
class vehikelbauer_t
{
public:
	static bool speedbonus_init(const std::string &objfilename);
	static sint32 get_speedbonus( sint32 monthyear, waytype_t wt );
	static void rdwr_speedbonus(loadsave_t *file);

	static bool register_desc(vehikel_desc_t *desc);
	static bool alles_geladen();

	static vehicle_t* baue(koord3d k, player_t* player, convoi_t* cnv, const vehikel_desc_t* vb )
	{
		return baue(k, player, cnv, vb, false);
	}

	static vehicle_t* baue(koord3d k, player_t* player, convoi_t* cnv, const vehikel_desc_t* vb, bool upgrade, uint16 livery_scheme_index = 0 );

	static const vehikel_desc_t * get_info(const char *name);
	static slist_tpl<vehikel_desc_t*>& get_info(waytype_t);

	/* extended search for vehicles for KI
	* @author prissi
	*/
	static const vehikel_desc_t *vehikel_search(waytype_t typ,const uint16 month_now,const uint32 target_power,const sint32 target_speed, const ware_desc_t * target_freight, bool include_electric, bool not_obsolete );

	/* for replacement during load time
	 * prev_veh==NULL equals leading of convoi
	 */
	static const vehikel_desc_t *get_best_matching( waytype_t wt, const uint16 month_now, const uint32 target_weight, const uint32 target_power, const sint32 target_speed, const ware_desc_t * target_freight, bool not_obsolete, const vehikel_desc_t *prev_veh, bool is_last );
};

#endif
