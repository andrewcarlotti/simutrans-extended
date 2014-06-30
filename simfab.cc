/*
 * Fabrikfunktionen und Fabrikbau
 *
 * Hansj�rg Malthaner
 *
 *
 * 25.03.00 Anpassung der Lagerkapazit�ten: min. 5 normale Lieferungen
 *          sollten an Lager gehalten werden.
 */

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simdebug.h"
#include "display/simimg.h"
#include "simcolor.h"
#include "simskin.h"
#include "boden/grund.h"
#include "boden/boden.h"
#include "boden/wege/strasse.h"
#include "boden/fundament.h"
#include "simfab.h"
#include "simcity.h"
#include "simhalt.h"
#include "simtools.h"
#include "simware.h"
#include "simworld.h"
#include "besch/haus_besch.h"
#include "besch/ware_besch.h"
#include "player/simplay.h"

#include "simmesg.h"
#include "simintr.h"

#include "obj/wolke.h"
#include "obj/gebaeude.h"
#include "obj/field.h"
#include "obj/leitung2.h"

#include "dataobj/settings.h"
#include "dataobj/environment.h"
#include "dataobj/translator.h"
#include "dataobj/loadsave.h"

#include "besch/fabrik_besch.h"
#include "bauer/hausbauer.h"
#include "bauer/warenbauer.h"
#include "bauer/fabrikbauer.h"

#include "gui/fabrik_info.h"

#include "utils/cbuffer_t.h"

#include "gui/simwin.h"
#include "display/simgraph.h"

#include "path_explorer.h"

#if MULTI_THREAD>1
#include <pthread.h>
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t add_to_world_list_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

// Fabrik_t


static const int FAB_MAX_INPUT = 15000;

karte_ptr_t fabrik_t::welt;


/**
 * Convert internal values to displayed values
 */
sint64 convert_goods(sint64 value) { return ( (value + (1<<(fabrik_t::precision_bits-1))) >> fabrik_t::precision_bits ); }
sint64 convert_power(sint64 value) { return ( value >> POWER_TO_MW ); }
sint64 convert_boost(sint64 value) { return ( (value * 100 + (DEFAULT_PRODUCTION_FACTOR>>1)) >> DEFAULT_PRODUCTION_FACTOR_BITS ); }


/**
 * Ordering based on relative distance to a fixed point `origin'.
 */
class RelativeDistanceOrdering
{
private:
	const koord m_origin;
public:
	RelativeDistanceOrdering(const koord& origin)
		: m_origin(origin)
	{ /* nothing */ }

	/**
	 * Returns true if `a' is closer to the origin than `b', otherwise false.
	 */
	bool operator()(const koord& a, const koord& b) const
	{
		return shortest_distance(m_origin, a) < shortest_distance(m_origin, b);
	}
};


void ware_production_t::init_stats()
{
	for(  int m=0;  m<MAX_MONTH;  ++m  ) {
		for(  int s=0;  s<MAX_FAB_GOODS_STAT;  ++s  ) {
			statistics[m][s] = 0;
		}
	}
	weighted_sum_storage = 0;
	transit = 0;
}


void ware_production_t::roll_stats(sint64 aggregate_weight)
{
	// calculate weighted average storage first
	if(  aggregate_weight>0  ) {
		set_stat( weighted_sum_storage / aggregate_weight, FAB_GOODS_STORAGE );
	}

	for(  int s=0;  s<MAX_FAB_GOODS_STAT;  ++s  ) {
		for(  int m=MAX_MONTH-1;  m>0;  --m  ) {
			statistics[m][s] = statistics[m-1][s];
		}
		if(  s==FAB_GOODS_TRANSIT  ) {
			// keep the current amount in transit
			statistics[0][s] = statistics[1][s];
		}
		else {
			statistics[0][s] = 0;
		}
	}
	weighted_sum_storage = 0;

	// restore current storage level
	set_stat( menge, FAB_GOODS_STORAGE );
}


void ware_production_t::rdwr(loadsave_t *file)
{
	if(  file->is_loading()  ) {
		init_stats();
		max_transit = 0;
	}

	if(  file->get_version()>112000  ) {
		for(  int s=0;  s<MAX_FAB_GOODS_STAT;  ++s  ) {
			for(  int m=0;  m<MAX_MONTH;  ++m  ) {
				file->rdwr_longlong( statistics[m][s] );
			}
		}
		file->rdwr_longlong( weighted_sum_storage );
#ifndef CACHE_TRANSIT
		// recalc transit always on load
		transit = 0;
		statistics[0][FAB_GOODS_TRANSIT] = 0;
#endif
	}
	else if(  file->get_version()>=110005  ) {
		// save/load statistics
		for(  int s=0;  s<3;  ++s  ) {
			for(  int m=0;  m<MAX_MONTH;  ++m  ) {
				file->rdwr_longlong( statistics[m][s] );
			}
		}
		file->rdwr_longlong( weighted_sum_storage );
	}
}


void ware_production_t::book_weighted_sum_storage(sint64 delta_time)
{
	weighted_sum_storage += (sint64)menge * delta_time;
	set_stat( menge, FAB_GOODS_STORAGE );
}


void fabrik_t::arrival_statistics_t::init()
{
	for(  uint32 s=0;  s<SLOT_COUNT;  ++s  ) {
		slots[s] = 0;
	}
	current_slot = 0;
	active_slots = 0;
	aggregate_arrival = 0;
	scaled_demand = 0;
}


void fabrik_t::arrival_statistics_t::rdwr(loadsave_t *file)
{
	if(  file->get_version()>=110005  ) {
		if(  file->is_loading()  ) {
			aggregate_arrival = 0;
			for(  uint32 s=0;  s<SLOT_COUNT;  ++s  ) {
				file->rdwr_short( slots[s] );
				aggregate_arrival += slots[s];
			}
			scaled_demand = 0;
		}
		else {
			for(  uint32 s=0;  s<SLOT_COUNT;  ++s  ) {
				file->rdwr_short( slots[s] );
			}
		}
		file->rdwr_short( current_slot );
		file->rdwr_short( active_slots );
	}
	else if(  file->is_loading()  ) {
		init();
	}
}


sint32 fabrik_t::arrival_statistics_t::advance_slot()
{
	sint32 result = 0;
	// advance to the next slot
	++current_slot;
	if(  current_slot>=SLOT_COUNT  ) {
		current_slot = 0;
	}
	// handle expiration of past arrivals and reset slot to 0
	if(  slots[current_slot]>0  ) {
		aggregate_arrival -= slots[current_slot];
		slots[current_slot] = 0;
		if(  aggregate_arrival==0  ) {
			// reset slot count to 0 as all previous arrivals have expired
			active_slots = 0;
		}
		result |= ARRIVALS_CHANGED;
	}
	// count the number of slots covered since aggregate arrival last increased from 0 to +ve
	if(  active_slots>0  &&  active_slots<SLOT_COUNT  ) {
		++active_slots;
		result |= ACTIVE_SLOTS_INCREASED;
	}
	return result;
}


void fabrik_t::arrival_statistics_t::book_arrival(const uint16 amount)
{
	if(  aggregate_arrival==0  ) {
		// new arrival after complete inactivity -> start counting slots
		active_slots = 1;
	}
	// increment current slot and aggregate arrival
	slots[current_slot] += amount;
	aggregate_arrival += amount;
}


void fabrik_t::update_transit( const ware_t& ware, bool add )
{
	if(  ware.index > warenbauer_t::INDEX_NONE  ) {
		// only for freights
		fabrik_t *fab = get_fab( ware.get_zielpos() );
		if(  fab  ) {
			fab->update_transit_intern( ware, add );
		}
	}
}


// just for simplicity ...
void fabrik_t::update_transit_intern( const ware_t& ware, bool add )
{
	FOR(  array_tpl<ware_production_t>,  &w,  eingang ) {
		if(  w.get_typ()->get_index() == ware.index  ) {
			if(  add  ) {
				w.transit += ware.menge;
			}
			else {
				w.transit -= ware.menge;
			}
			w.set_stat( w.transit, FAB_GOODS_TRANSIT );
			return;
		}
	}
}


void fabrik_t::init_stats()
{
	for(  int m=0;  m<MAX_MONTH;  ++m  ) {
		for(  int s=0;  s<MAX_FAB_STAT;  ++s  ) {
			statistics[m][s] = 0;
		}
	}
	weighted_sum_production = 0;
	weighted_sum_boost_electric = 0;
	weighted_sum_boost_pax = 0;
	weighted_sum_boost_mail = 0;
	weighted_sum_power = 0;
	aggregate_weight = 0;
}


void fabrik_t::book_weighted_sums(sint64 delta_time)
{
	aggregate_weight += delta_time;

	// storage level of input/output stores
	FOR(array_tpl<ware_production_t>, & g, eingang) {
		g.book_weighted_sum_storage(delta_time);
	}
	FOR(array_tpl<ware_production_t>, & g, ausgang) {
		g.book_weighted_sum_storage(delta_time);
	}

	// production level
	const sint32 current_prod = get_current_production();
	weighted_sum_production += current_prod * delta_time;
	set_stat( current_prod, FAB_PRODUCTION );

	// electricity, pax and mail boosts
	weighted_sum_boost_electric += prodfactor_electric * delta_time;
	set_stat( prodfactor_electric, FAB_BOOST_ELECTRIC );
	weighted_sum_boost_pax += prodfactor_pax * delta_time;
	weighted_sum_boost_mail += prodfactor_mail * delta_time;

	// power produced or consumed
	weighted_sum_power += power * delta_time;
	set_stat( power, FAB_POWER );
}


void fabrik_t::update_scaled_electric_amount()
{
	if(besch->get_electric_amount() == 65535) 
	{
		// demand not specified in pak, use old fixed demands and the Experimental electricity proportion
		const uint16 electricity_proportion = get_besch()->is_electricity_producer() ? 400 : get_besch()->get_electricity_proportion();
		scaled_electric_amount = PRODUCTION_DELTA_T * (uint32)(prodbase * electricity_proportion) / 100;
		return;
	}

	// If the demand is specified in the pakset, do not use the Experimental electricity proportion.
	const sint64 prod = besch->get_produktivitaet();
	scaled_electric_amount = (uint32)( (( (sint64)(besch->get_electric_amount()) * (sint64)prodbase + (prod >> 1) ) / prod) << POWER_TO_MW );

	if(  scaled_electric_amount == 0  ) {
		prodfactor_electric = 0;
	}
}


void fabrik_t::update_scaled_pax_demand()
{
	if(!welt->get_is_shutting_down())
	{
		// first, scaling based on current production base
		const sint64 prod = besch->get_produktivitaet() > 0 ? besch->get_produktivitaet() : 1;
		gebaeude_t* gb = get_building();

		const sint64 base_pax_demand = (!gb || gb->get_tile()->get_besch()->get_employment_capacity() == 65535) ? (besch->get_pax_demand() == 65535 ? besch->get_pax_level() : besch->get_pax_demand()) : gb->get_jobs();
		// formula : base_pax_demand * (current_production_base / besch_production_base); (prod >> 1) is for rounding
		const uint32 pax_demand = (uint32)( ( base_pax_demand * (sint64)prodbase + (prod >> 1) ) / prod );
		// then, scaling based on month length
		scaled_pax_demand = max(welt->calc_adjusted_monthly_figure(pax_demand), 1);

		// pax demand for fixed period length
		// It is intended that pax_demand, not scaled_pax_demand be used here despite the method name.
		arrival_stats_pax.set_scaled_demand( pax_demand );

		// Must update the world building list to take into account the new passenger demand (weighting)
		if(gb)
		{
			welt->update_weight_of_building_in_world_list(gb);
		}
	}
}


void fabrik_t::update_scaled_mail_demand()
{
	if(!welt->get_is_shutting_down())
	{
		// first, scaling based on current production base
		const sint64 prod = besch->get_produktivitaet() > 0 ? besch->get_produktivitaet() : 1;
		gebaeude_t* gb = get_building();

		const sint64 base_mail_demand = (!gb || gb->get_tile()->get_besch()->get_mail_demand_and_production_capacity() == 65535) ? (besch->get_mail_demand() == 65535 ? (besch->get_pax_level() >> 2) : besch->get_mail_demand()) : gb->get_mail_demand();
		// formula : besch_mail_demand * (current_production_base / besch_production_base); (prod >> 1) is for rounding
		const uint32 mail_demand = (uint32)( ( base_mail_demand * (sint64)prodbase + (prod >> 1) ) / prod );
		// then, scaling based on month length
		scaled_mail_demand = max(welt->calc_adjusted_monthly_figure(mail_demand), 1);

		// mail demand for fixed period length
		// It is intended that mail_demand, not scaled_mail_demand be used here despite the method name
		arrival_stats_mail.set_scaled_demand( mail_demand );

		// Must update the world building list to take into account the new passenger demand (weighting)
		if(gb)
		{
			welt->update_weight_of_building_in_world_list(gb);
		}
	}
}


void fabrik_t::update_prodfactor_pax()
{
	// calculate pax boost based on arrival data and demand of the fixed-length period
	const uint32 periods = welt->get_settings().get_factory_arrival_periods();
	const uint32 slots = arrival_stats_pax.get_active_slots();
	const uint32 pax_demand = ( periods==1 || slots*periods<=(uint32)SLOT_COUNT ?
									arrival_stats_pax.get_scaled_demand() :
									( slots==(uint32)SLOT_COUNT ?
										arrival_stats_pax.get_scaled_demand() * periods :
										(arrival_stats_pax.get_scaled_demand() * periods * slots) >> SLOT_BITS ) );
	const uint32 pax_arrived = arrival_stats_pax.get_aggregate_arrival();
	if(  pax_demand==0  ||  pax_arrived==0  ||  besch->get_pax_boost()==0  ) {
		prodfactor_pax = 0;
	}
	else if(  pax_arrived>=pax_demand  ) {
		// maximum boost
		prodfactor_pax = besch->get_pax_boost();
	}
	else {
		// pro-rata boost : (pax_arrived / pax_demand) * besch_pax_boost; (pax_demand >> 1) is for rounding
		prodfactor_pax = (sint32)( ( (sint64)pax_arrived * (sint64)(besch->get_pax_boost()) + (sint64)(pax_demand >> 1) ) / (sint64)pax_demand );
	}
	set_stat(prodfactor_pax, FAB_BOOST_PAX);
}


void fabrik_t::update_prodfactor_mail()
{
	// calculate mail boost based on arrival data and demand of the fixed-length period
	const uint32 periods = welt->get_settings().get_factory_arrival_periods();
	const uint32 slots = arrival_stats_mail.get_active_slots();
	const uint32 mail_demand = ( periods==1 || slots*periods<=(uint32)SLOT_COUNT ?
									arrival_stats_mail.get_scaled_demand() :
									( slots==(uint32)SLOT_COUNT ?
										arrival_stats_mail.get_scaled_demand() * periods :
										(arrival_stats_mail.get_scaled_demand() * periods * slots) >> SLOT_BITS ) );
	const uint32 mail_arrived = arrival_stats_mail.get_aggregate_arrival();
	if(  mail_demand==0  ||  mail_arrived==0  ||  besch->get_mail_boost()==0  ) {
		prodfactor_mail = 0;
	}
	else if(  mail_arrived>=mail_demand  ) {
		// maximum boost
		prodfactor_mail = besch->get_mail_boost();
	}
	else {
		// pro-rata boost : (mail_arrived / mail_demand) * besch_mail_boost; (mail_demand >> 1) is for rounding
		prodfactor_mail = (sint32)( ( (sint64)mail_arrived * (sint64)(besch->get_mail_boost()) + (sint64)(mail_demand >> 1) ) / (sint64)mail_demand );
	}
	set_stat(prodfactor_mail, FAB_BOOST_MAIL);
}


void fabrik_t::recalc_storage_capacities()
{
	if(  besch->get_field_group()  ) {
		// with fields -> calculate based on capacities contributed by fields
		const uint32 ware_types = eingang.get_count() + ausgang.get_count();
		if(  ware_types>0  ) {
			// calculate total storage capacity contributed by fields
			const field_group_besch_t *const field_group = besch->get_field_group();
			sint32 field_capacities = 0;
			FOR(vector_tpl<field_data_t>, const& f, fields) {
				field_capacities += field_group->get_field_class(f.field_class_index)->get_storage_capacity();
			}
			const sint32 share = (sint32)( ( (sint64)field_capacities << precision_bits ) / (sint64)ware_types );
			// first, for input goods
			FOR(array_tpl<ware_production_t>, & g, eingang) {
				for(  int b=0;  b<besch->get_lieferanten();  ++b  ) {
					const fabrik_lieferant_besch_t *const input = besch->get_lieferant(b);
					if (g.get_typ() == input->get_ware()) {
						g.max = (input->get_kapazitaet() << precision_bits) + share;
					}
				}
			}
			// then, for output goods
			FOR(array_tpl<ware_production_t>, & g, ausgang) {
				for(  uint b=0;  b<besch->get_produkte();  ++b  ) {
					const fabrik_produkt_besch_t *const output = besch->get_produkt(b);
					if (g.get_typ() == output->get_ware()) {
						g.max = (output->get_kapazitaet() << precision_bits) + share;
					}
				}
			}
		}
	}
	else {
		// without fields -> scaling based on prodbase
		// first, for input goods
		FOR(array_tpl<ware_production_t>, & g, eingang) {
			for(  int b=0;  b<besch->get_lieferanten();  ++b  ) {
				const fabrik_lieferant_besch_t *const input = besch->get_lieferant(b);
				if (g.get_typ() == input->get_ware()) {
					g.max = (sint32)((sint64)(input->get_kapazitaet() << precision_bits) * (sint64)prodbase / (sint64)besch->get_produktivitaet());
				}
			}
		}
		// then, for output goods
		FOR(array_tpl<ware_production_t>, & g, ausgang) {
			for(  uint b=0;  b<besch->get_produkte();  ++b  ) {
				const fabrik_produkt_besch_t *const output = besch->get_produkt(b);
				if (g.get_typ() == output->get_ware()) {
					g.max = (sint32)((sint64)(output->get_kapazitaet() << precision_bits) * (sint64)prodbase / (sint64)besch->get_produktivitaet());
				}
			}
		}
	}
}

void fabrik_t::set_base_production(sint32 p)
{
	prodbase = p > 0 ? p : 1;
	recalc_storage_capacities();
	update_scaled_electric_amount();
	update_scaled_pax_demand();
	update_scaled_mail_demand();
	update_prodfactor_pax();
	update_prodfactor_mail();
	calc_max_intransit_percentages();
}


fabrik_t *fabrik_t::get_fab(const koord &pos)
{
	const grund_t *gr = welt->lookup_kartenboden(pos);
	if(gr) {
		gebaeude_t *gb = gr->find<gebaeude_t>();
		if(gb) {
			return gb->get_fabrik();
		}
	}
	return NULL;
}


//void fabrik_t::link_halt(halthandle_t halt)
//{
//	welt->access(pos.get_2d())->add_to_haltlist(halt);
//}
//
//
//void fabrik_t::unlink_halt(halthandle_t halt)
//{
//	planquadrat_t *plan=welt->access(pos.get_2d());
//	if(plan) {
//		plan->remove_from_haltlist(halt);
//	}
//}


void fabrik_t::add_lieferziel(koord ziel)
{
	if(  !lieferziele.is_contained(ziel)  ) {
		lieferziele.insert_ordered( ziel, RelativeDistanceOrdering(pos.get_2d()) );
		// now tell factory too
		fabrik_t * fab = fabrik_t::get_fab(ziel);
		if (fab) {
			fab->add_supplier(get_pos().get_2d());
		}
	}
}


void fabrik_t::rem_lieferziel(koord ziel)
{
	lieferziele.remove(ziel);
}

bool
fabrik_t::disconnect_consumer(koord pos) //Returns true if must be destroyed.
{
	rem_lieferziel(pos);
	if(lieferziele.get_count() < 1)
	{
		// If there are no consumers left, industry is orphaned.
		// Reconnect or close.

		// Attempt to reconnect. NOTE: This code may not work well if there are multiple supply types.
		
		for(sint16 i = welt->get_fab_list().get_count() - 1; i >= 0; i --)
		{
			fabrik_t* fab = welt->get_fab_list()[i];
			if(add_customer(fab)) 
			{
				//Only reconnect one.
				return false;
			}
		}
		return true;
	}
	return false;
}

bool
fabrik_t::disconnect_supplier(koord pos) //Returns true if must be destroyed.
{
	rem_supplier(pos);
	if(suppliers.empty())
	{
		// If there are no suppliers left, industry is orphaned.
		// Reconnect or close.

		// Attempt to reconnect. NOTE: This code may not work well if there are multiple supply types.
		
		for(sint16 i = welt->get_fab_list().get_count() - 1; i >= 0; i --)
		{
			fabrik_t* fab = welt->get_fab_list()[i];
			if(add_supplier(fab))
			{
				//Only reconnect one.
				return false;
			}
		}
		return true;
	}
	return false;
}


fabrik_t::fabrik_t(loadsave_t* file)
{
	besitzer_p = NULL;
	power = 0;
	power_demand = 0;
	prodfactor_electric = 0;
	lieferziele_active_last_month = 0;
	city = NULL;
	building = NULL;
	pos = koord3d::invalid;

	rdwr(file);

	delta_sum = 0;
	delta_menge = 0;
	menge_remainder = 0;
	total_input = total_transit = total_output = 0;
	status = nothing;
	currently_producing = false;
	transformer_connected = NULL;

	if(  besch == NULL  ) {
		dbg->warning( "fabrik_t::fabrik_t()", "No pak-file for factory at (%s) - will not be built!", pos_origin.get_str() );
		return;
	}
	else if(  !welt->is_within_limits(pos_origin.get_2d())  ) {
		dbg->warning( "fabrik_t::fabrik_t()", "%s is not a valid position! (Will not be built!)", pos_origin.get_str() );
		besch = NULL; // to get rid of this broken factory later...
	}
	else {
		baue(rotate, false, false);
		// now get rid of construction image
		for(  sint16 y=0;  y<besch->get_haus()->get_h(rotate);  y++  ) {
			for(  sint16 x=0;  x<besch->get_haus()->get_b(rotate);  x++  ) {
				gebaeude_t *gb = welt->lookup_kartenboden( pos_origin.get_2d()+koord(x,y) )->find<gebaeude_t>();
				if(  gb  ) {
					gb->add_alter(10000ll);
				}
			}
		}
		// Must rebuild the nearby halt database
		recalc_nearby_halts();
	}
}


fabrik_t::fabrik_t(koord3d pos_, spieler_t* spieler, const fabrik_besch_t* fabesch, sint32 initial_prod_base) :
	besch(fabesch),
	pos(pos_)
{
	pos.z = welt->max_hgt(pos.get_2d());
	pos_origin = pos;
	building = NULL;

	besitzer_p = spieler;

	prodfactor_electric = 0;
	prodfactor_pax = 0;
	prodfactor_mail = 0;
	if (initial_prod_base < 0) {
		prodbase = besch->get_produktivitaet() + simrand(besch->get_bereich(), "fabrik_t::fabrik_t() prodbase");
	}
	else {
		prodbase = initial_prod_base;
	}

	delta_sum = 0;
	delta_menge = 0;
	menge_remainder = 0;
	activity_count = 0;
	currently_producing = false;
	transformer_connected = NULL;
	power = 0;
	power_demand = 0;
	total_input = total_transit = total_output = 0;
	status = nothing;
	lieferziele_active_last_month = 0;
	city = welt->get_city(pos.get_2d());
	if(city != NULL)
	{
		city->add_city_factory(this);
		city->update_city_stats_with_building(get_building(), false);
	}

	if(fabesch->get_platzierung() == 2 && city && fabesch->get_produkte() == 0)
	{
		// City consumer industries set their consumption rates by the relative size of the city
		const weighted_vector_tpl<stadt_t*>& cities = welt->get_staedte();

		sint64 biggest_city_population = 0;
		sint64 smallest_city_population = -1;

		for (weighted_vector_tpl<stadt_t*>::const_iterator i = cities.begin(), end = cities.end(); i != end; ++i)
		{
			stadt_t* const c = *i;
			const sint64 pop = c->get_finance_history_month(0,HIST_CITICENS);
			if(pop > biggest_city_population)
			{
				biggest_city_population = pop;
			}
			else if(pop < smallest_city_population || smallest_city_population == -1)
			{
				smallest_city_population = pop;
			}
		}

		const sint64 this_city_population = city->get_finance_history_month(0,HIST_CITICENS);
		sint32 production;

		if(this_city_population == biggest_city_population)
		{
			production = besch->get_bereich();
		}
		else if(this_city_population == smallest_city_population)
		{
			production = 0;
		}
		else
		{
			const int percentage = (this_city_population - smallest_city_population) * 100 / (biggest_city_population - smallest_city_population);
			production = (besch->get_bereich() * percentage) / 100;
		}
		prodbase = besch->get_produktivitaet() + production;
	}
	else if(fabesch->get_platzierung() == 2 && !city && fabesch->get_produkte() == 0)
	{
		prodbase = besch->get_produktivitaet();
	}
	else
	{
		prodbase = besch->get_produktivitaet() + simrand(besch->get_bereich(), "fabrik_t::fabrik_t");
	}
	
	prodbase = prodbase > 0 ? prodbase : 1;

	// create input information
	eingang.resize( fabesch->get_lieferanten() );
	for(  int g=0;  g<fabesch->get_lieferanten();  ++g  ) {
		const fabrik_lieferant_besch_t *const input = fabesch->get_lieferant(g);
		eingang[g].set_typ( input->get_ware() );
	}

	// create output information
	ausgang.resize( fabesch->get_produkte() );
	for(  uint g=0;  g<fabesch->get_produkte();  ++g  ) {
		const fabrik_produkt_besch_t *const product = fabesch->get_produkt(g);
		ausgang[g].set_typ( product->get_ware() );
	}

	recalc_storage_capacities();
	if (eingang.empty()) {
		FOR(array_tpl<ware_production_t>, & g, ausgang) {
			if (g.max > 0) {
				// if source then start with full storage, so that AI will build line(s) immediately
				g.menge = g.max - 1;
			}
		}
	}
	
	init_stats();
	arrival_stats_pax.init();
	arrival_stats_mail.init();

	delta_slot = 0;
	times_expanded = 0;

	calc_max_intransit_percentages();

	update_scaled_electric_amount();
	update_scaled_pax_demand();
	update_scaled_mail_demand();

	// We can't do these here, because get_tile_list will fail
	// We have to wait until after ::baue is called
	// It would be better to call ::baue here, but that fails too
	// --neroden
	// mark_connected_roads(false);
	// recalc_nearby_halts();
}

void fabrik_t::mark_connected_roads(bool del)
{
	grund_t* gr;
	vector_tpl<koord> tile_list;
	get_tile_list(tile_list);
	FOR(vector_tpl<koord>, const k, tile_list)
	{
		for(uint8 i = 0; i < 8; i ++)
		{
			// Check for connected roads. Only roads in immediately neighbouring tiles
			// and only those on the same height will register a connexion.
			koord3d pos3d(k + k.neighbours[i], pos.z);
			gr = welt->lookup(pos3d);
			if(!gr)
			{
				continue;
			}
			strasse_t* str = (strasse_t*)gr->get_weg(road_wt);
			gebaeude_t* gb = gr->find<gebaeude_t>();
			if(str)
			{
				if(del)
				{
					str->connected_buildings.remove(gb);
				}
				else
				{
					str->connected_buildings.append_unique(gb);
				}
			}
		}
	}
}

void fabrik_t::delete_all_fields()
{
	while(!fields.empty()) 
	{
		planquadrat_t *plan = welt->access( fields.back().location );
		// if destructor is called when world is destroyed, plan is already invalid
		if (plan) {
			grund_t *gr = plan->get_kartenboden();
			if (field_t* f = gr->find<field_t>()) {
				delete f; // implicitly removes the field from fields
				plan->boden_ersetzen( gr, new boden_t(gr->get_pos(), hang_t::flach ) );
				plan->get_kartenboden()->calc_bild();
				continue;
			}
		}
		fields.pop_back();
	}
	// destroy chart window, if present
	destroy_win((ptrdiff_t)this);
}

fabrik_t::~fabrik_t()
{
	mark_connected_roads(true);
	delete_all_fields();

	if(city != NULL)
	{
		city->remove_city_factory(this);
	}

	welt->remove_building_from_world_list(get_building());

	if(!welt->get_is_shutting_down())
	{
		if (besch != NULL)
		{
			welt->decrease_actual_industry_density(100 / besch->get_gewichtung());
		}

		// Disconnect this factory from all chains.
		// @author: jamespetts
		uint32 number_of_customers = lieferziele.get_count();
		uint32 number_of_suppliers = suppliers.get_count();
		const weighted_vector_tpl<stadt_t*>& staedte = welt->get_staedte();
		for(weighted_vector_tpl<stadt_t*>::const_iterator j = staedte.begin(), end = staedte.end(); j != end; ++j) 
		{
			(*j)->remove_connected_industry(this);
		}
		
		char buf[192];
		sprintf(buf, translator::translate("Industry:\n%s\nhas closed,\nwith the loss\nof %d jobs.\n%d upstream\nsuppliers and\n%d downstream\ncustomers\nare affected."), translator::translate(get_name()), get_base_pax_demand(), number_of_suppliers, number_of_customers);
		welt->get_message()->add_message(buf, pos.get_2d(), message_t::industry, COL_DARK_RED, skinverwaltung_t::neujahrsymbol->get_bild_nr(0));
		for(sint32 i = number_of_customers - 1; i >= 0; i --)
		{
			fabrik_t* tmp = get_fab(lieferziele[i]);
			if(tmp && tmp->disconnect_supplier(pos.get_2d()))
			{
				// Orphaned, must be deleted.
				gebaeude_t* gb = tmp->get_building();
				hausbauer_t::remove(welt->get_spieler(1), gb);
			}
		}

		for(sint32 i = number_of_suppliers - 1; i >= 0; i --)
		{
			fabrik_t* tmp = get_fab(suppliers[i]);
			if(tmp && tmp->disconnect_consumer(pos.get_2d()))
			{
				// Orphaned, must be deleted.
				gebaeude_t* gb = tmp->get_building();
				hausbauer_t::remove(welt->get_spieler(1), gb);
			}
		}
		if(transformer_connected)
		{
			transformer_connected->clear_factory();
		}
	}
}


void fabrik_t::baue(sint32 rotate, bool build_fields, bool force_initial_prodbase)
{
	this->rotate = rotate;
	pos_origin = welt->lookup_kartenboden(pos_origin.get_2d())->get_pos();
	if(!building)
	{
 		building = hausbauer_t::baue(besitzer_p, pos_origin, rotate, besch->get_haus(), this);
	}
	pos = building->get_pos();
	pos_origin.z = pos.z;

	if(besch->get_field_group()) {
		// if there are fields
		if(  !fields.empty()  ) {
			for(  uint16 i=0;  i<fields.get_count();  i++   ) {
				const koord k = fields[i].location;
				grund_t *gr=welt->lookup_kartenboden(k);
				if(  gr->ist_natur()  ) {
					// first make foundation below
					grund_t *gr2 = new fundament_t(gr->get_pos(), gr->get_grund_hang());
					welt->access(k)->boden_ersetzen(gr, gr2);
					gr2->obj_add( new field_t(gr2->get_pos(), besitzer_p, besch->get_field_group()->get_field_class( fields[i].field_class_index ), this ) );
				}
				else {
					// there was already a building at this position => do not restore!
					fields.remove_at(i);
					i--;
				}
			}
		}
		else if(  build_fields  ) {
			// make sure not to exceed initial prodbase too much
			sint32 org_prodbase = prodbase;
			// we will start with a minimum number and try to get closer to start_fields
			const field_group_besch_t& field_group = *besch->get_field_group();
			const uint16 spawn_fields = field_group.get_min_fields() + simrand( field_group.get_start_fields() - field_group.get_min_fields(), "fabrik_t::baue" );
			while(  fields.get_count() < spawn_fields  &&  add_random_field(10000u)  ) {
				if (fields.get_count() > besch->get_field_group()->get_min_fields()  &&  prodbase >= 2*org_prodbase) {
					// too much productivity, no more fields needed
					break;
				}
			}
			sint32 field_prod = prodbase - org_prodbase;
			// adjust prodbase
			if (force_initial_prodbase) {
				set_base_production( max(field_prod, org_prodbase) );
			}
		}
	}
}


/* field generation code
 * @author Kieron Green
 */
bool fabrik_t::add_random_field(uint16 probability)
{
	// has fields, and not yet too many?
	const field_group_besch_t *fb = besch->get_field_group();
	if(fb==NULL  ||  fb->get_max_fields() <= fields.get_count()) {
		return false;
	}
	// we are lucky and are allowed to generate a field
	if(simrand(10000, "bool fabrik_t::add_random_field")>=probability) {
		return false;
	}

	// we start closest to the factory, and check for valid tiles as we move out
	uint8 radius = 1;

	// pick a coordinate to use - create a list of valid locations and choose a random one
	slist_tpl<grund_t *> build_locations;
	do {
		for(sint32 xoff = -radius; xoff < radius + get_besch()->get_haus()->get_groesse().x ; xoff++) {
			for(sint32 yoff =-radius ; yoff < radius + get_besch()->get_haus()->get_groesse().y; yoff++) {
				// if we can build on this tile then add it to the list
				grund_t *gr = welt->lookup_kartenboden(pos.get_2d()+koord(xoff,yoff));
				if (gr != NULL &&
						gr->get_typ()        == grund_t::boden &&
						gr->get_hoehe()      == pos.z &&
						gr->get_grund_hang() == hang_t::flach &&
						gr->ist_natur() &&
						(gr->find<leitung_t>() || gr->kann_alle_obj_entfernen(NULL) == NULL)) {
					// only on same height => climate will match!
					build_locations.append(gr);
					assert(gr->find<field_t>() == NULL);
				}
				// skip inside of rectange (already checked earlier)
				if(radius > 1 && yoff == -radius && (xoff > -radius && xoff < radius + get_besch()->get_haus()->get_groesse().x - 1)) {
					yoff = radius + get_besch()->get_haus()->get_groesse().y - 2;
				}
			}
		}
		if (build_locations.empty()) {
			radius++;
		}
	} while (radius < 10 && build_locations.empty());
	// built on one of the positions
	if (!build_locations.empty()) {
		grund_t *gr = build_locations.at(simrand(build_locations.get_count(), "bool fabrik_t::add_random_field"));
		leitung_t* lt = gr->find<leitung_t>();
		if(lt) {
			gr->obj_remove(lt);
		}
		gr->obj_loesche_alle(NULL);
		// first make foundation below
		const koord k = gr->get_pos().get_2d();
		field_data_t new_field(k);
		assert(!fields.is_contained(new_field));
		// Knightly : fetch a random field class besch based on spawn weights
		const weighted_vector_tpl<uint16> &field_class_indices = fb->get_field_class_indices();
		new_field.field_class_index = pick_any_weighted(field_class_indices);
		const field_class_besch_t *const field_class = fb->get_field_class( new_field.field_class_index );
		fields.append(new_field);
		grund_t *gr2 = new fundament_t(gr->get_pos(), gr->get_grund_hang());
		welt->access(k)->boden_ersetzen(gr, gr2);
		gr2->obj_add( new field_t(gr2->get_pos(), besitzer_p, field_class, this ) );
		// Knightly : adjust production base and storage capacities
		set_base_production( prodbase + field_class->get_field_production() );
		if(lt) {
			gr2->obj_add( lt );
		}
		gr2->calc_bild();
		return true;
	}
	return false;
}


void fabrik_t::remove_field_at(koord pos)
{
	field_data_t field(pos);
	assert(fields.is_contained( field ));
	field = fields[ fields.index_of(field) ];
	const field_class_besch_t *const field_class = besch->get_field_group()->get_field_class( field.field_class_index );
	fields.remove(field);
	// Knightly : revert the field's effect on production base and storage capacities
	set_base_production( prodbase - field_class->get_field_production() );
}


//bool fabrik_t::ist_bauplatz(karte_t *welt, koord pos, koord groesse,bool wasser,climate_bits cl)
//{
//	if(pos.x > 0 && pos.y > 0 &&
//		pos.x+groesse.x < welt->get_size().x && pos.y+groesse.y < welt->get_size().y &&
//		( wasser  ||  welt->square_is_free(pos, groesse.x, groesse.y, NULL, cl) )&&
//		!ist_da_eine(welt,pos-koord(5,5),pos+groesse+koord(3,3))) {
//
//		// check for water (no shore in sight!)
//		if(wasser) {
//			for(int y=0;y<groesse.y;y++) {
//				for(int x=0;x<groesse.x;x++) {
//					const grund_t *gr=welt->lookup_kartenboden(pos+koord(x,y));
//					if(!gr->ist_wasser()  ||  gr->get_grund_hang()!=hang_t::flach) {
//						return false;
//					}
//				}
//			}
//		}
//
//		return true;
//	}
//	return false;
//}

// "Are there any?" (Google Translate)
vector_tpl<fabrik_t *> &fabrik_t::sind_da_welche(koord min_pos, koord max_pos)
{
	static vector_tpl <fabrik_t*> fablist(16);
	fablist.clear();

	for(int y=min_pos.y; y<=max_pos.y; y++) {
		for(int x=min_pos.x; x<=max_pos.x; x++) {
			fabrik_t *fab=get_fab(koord(x,y));
			if(fab) {
				if (fablist.append_unique(fab)) {
//DBG_MESSAGE("fabrik_t::sind_da_welche()","appended factory %s at (%i,%i)",gr->first_obj()->get_fabrik()->get_besch()->get_name(),x,y);
				}
			}
		}
	}
	return fablist;
}


/**
 * if name==NULL translate besch factory name in game language
 */
char const* fabrik_t::get_name() const
{
	return name ? name.c_str() : translator::translate(besch->get_name(), welt->get_settings().get_name_language_id());
}


void fabrik_t::set_name(const char *new_name)
{
	if(new_name==NULL  ||  strcmp(new_name, translator::translate(besch->get_name(), welt->get_settings().get_name_language_id()))==0) {
		// new name is equal to name given by besch/translation -> set name to NULL
		name = NULL;
	} else {
		name = new_name;
	}

	fabrik_info_t *win = dynamic_cast<fabrik_info_t*>(win_get_magic((ptrdiff_t)this));
	if (win) {
		win->update_info();
	}
}


void fabrik_t::rdwr(loadsave_t *file)
{
	xml_tag_t f( file, "fabrik_t" );
	sint32 i;
	sint32 spieler_n;
	sint32 eingang_count;
	sint32 ausgang_count;
	sint32 anz_lieferziele;

	if(  file->is_saving()  ) {
		eingang_count = eingang.get_count();
		ausgang_count = ausgang.get_count();
		anz_lieferziele = lieferziele.get_count();
		const char *s = besch->get_name();
		file->rdwr_str(s);
	}
	else {
		char s[256];
		file->rdwr_str(s, lengthof(s));
DBG_DEBUG("fabrik_t::rdwr()","loading factory '%s'",s);
		besch = fabrikbauer_t::get_fabesch(s);
		if(  besch==NULL  ) {
			//  maybe it was only renamed?
			besch = fabrikbauer_t::get_fabesch(translator::compatibility_name(s));
		}
		if(  besch==NULL  ) {
			dbg->warning( "fabrik_t::rdwr()", "Pak-file for factory '%s' missing!", s );
			// we continue loading even if besch==NULL
			welt->add_missing_paks( s, karte_t::MISSING_FACTORY );
		}
	}
	pos_origin.rdwr(file);
	// pos will be assigned after call to hausbauer_t::baue
	file->rdwr_byte(rotate);

	// now rebuilt information for received goods
	file->rdwr_long(eingang_count);
	if(  file->is_loading()  ) {
		eingang.resize( eingang_count );
	}
	for(  i=0;  i<eingang_count;  ++i  ) {
		ware_production_t &ware = eingang[i];
		const char *ware_name = NULL;
		if(  file->is_saving()  ) {
			ware_name = ware.get_typ()->get_name();
		}
		file->rdwr_str(ware_name);
		file->rdwr_long(ware.menge);
		if(  file->get_version()<110005  ) {
			// max storage is only loaded/saved for older versions
			file->rdwr_long(ware.max);
		}
		ware.rdwr( file );
		if(  file->is_loading()  ) {
			ware.set_typ( warenbauer_t::get_info(ware_name) );
			guarded_free(const_cast<char *>(ware_name));
			// Hajo: repair files that have 'insane' values
			if(  ware.menge<0  ) {
				ware.menge = 0;
			}
			if(  ware.menge>(FAB_MAX_INPUT<<precision_bits)  ) {
				ware.menge = (FAB_MAX_INPUT << precision_bits);
			}
			/*
			* It's very easy for in-transit information to get corrupted,
			* if an intermediate program version fails to compute it right.
			* So *always* compute it fresh.  Do NOT load it.
			* It will be recomputed by halts and vehicles.
			*
			* Note, for this to work factories must be loaded before halts and vehicles
			* (this is how it is currently done in simworld.cc)
			*/
			// ware.transit = ware.get_stat( 0, FAB_GOODS_TRANSIT );
		}
	}

	// now rebuilt information for produced goods
	file->rdwr_long(ausgang_count);
	if(  file->is_loading()  ) {
		ausgang.resize( ausgang_count );
	}
	for(  i=0;  i<ausgang_count;  ++i  ) {
		ware_production_t &ware = ausgang[i];
		const char *ware_name = NULL;
		if(  file->is_saving()  ) {
			ware_name = ware.get_typ()->get_name();
		}
		file->rdwr_str(ware_name);
		file->rdwr_long(ware.menge);
		if(  file->get_version()<110005  ) {
			// max storage is only loaded/saved for older versions
			file->rdwr_long(ware.max);
			// obsolete variables -> statistics already contain records on goods delivered
			sint32 abgabe_sum = (sint32)(ware.get_stat(0, FAB_GOODS_DELIVERED));
			sint32 abgabe_letzt = (sint32)(ware.get_stat(1, FAB_GOODS_DELIVERED));
			file->rdwr_long(abgabe_sum);
			file->rdwr_long(abgabe_letzt);
		}
		ware.rdwr( file );
		if(  file->is_loading()  ) {
			ware.set_typ( warenbauer_t::get_info(ware_name) );
			guarded_free(const_cast<char *>(ware_name));
			// Hajo: repair files that have 'insane' values
			if(  ware.menge<0  ) {
				ware.menge = 0;
			}
		}
	}

	// restore other information
	spieler_n = welt->sp2num(besitzer_p);
	file->rdwr_long(spieler_n);
	file->rdwr_long(prodbase);
	if(  file->get_version()<110005  ) {
		// TurfIt : prodfaktor saving no longer required
		sint32 adjusted_value = (prodfactor_electric / 16) + 16;
		file->rdwr_long(adjusted_value);
	}

	if(  file->get_version() > 99016  ) {
		file->rdwr_long(power);
	}

	// owner stuff
	if(  file->is_loading()  ) {
		// take care of old files
		if(  file->get_version() < 86001  ) {
			koord k = besch ? besch->get_haus()->get_groesse() : koord(1,1);
			DBG_DEBUG("fabrik_t::rdwr()","correction of production by %i",k.x*k.y);
			// since we step from 86.01 per factory, not per tile!
			prodbase *= k.x*k.y*2;
		}
		// Hajo: restore factory owner
		// Due to a omission in Volkers changes, there might be savegames
		// in which factories were saved without an owner. In this case
		// set the owner to the default of player 1
		if(spieler_n == -1) {
			// Use default
			besitzer_p = welt->get_spieler(1);
		}
		else {
			// Restore owner pointer
			besitzer_p = welt->get_spieler(spieler_n);
		}
	}

	file->rdwr_long(anz_lieferziele);

	// connect/save consumer
	for(int i=0; i<anz_lieferziele; i++) {
		if(file->is_loading()) {
			lieferziele.append(koord::invalid);
		}
		lieferziele[i].rdwr(file);
	}

	if(  file->get_version()>=112002  ) {
		file->rdwr_long( lieferziele_active_last_month );
	}

	// suppliers / consumers will be recalculated in laden_abschliessen
	if (file->is_loading()  &&  welt->get_settings().is_crossconnect_factories()) {
		lieferziele.clear();
	}

	// information on fields ...
	if(  file->get_version() > 99009  ) {
		if(  file->is_saving()  ) {
			uint16 nr=fields.get_count();
			file->rdwr_short(nr);
			if(  file->get_version()>102002  && file->get_experimental_version() != 7 ) {
				// each field stores location and a field class index
				for(  uint16 i=0  ;  i<nr  ;  ++i  ) {
					koord k = fields[i].location;
					k.rdwr(file);
					uint16 idx = fields[i].field_class_index;
					file->rdwr_short(idx);
				}
			}
			else {
				// each field only stores location
				for(  uint16 i=0  ;  i<nr  ;  ++i  ) {
					koord k = fields[i].location;
					k.rdwr(file);
				}
			}
		}
		else {
			uint16 nr=0;
			koord k;
			file->rdwr_short(nr);
			fields.resize(nr);
			if(  file->get_version()>102002  && file->get_experimental_version() != 7 ) {
				// each field stores location and a field class index
				for(  uint16 i=0  ;  i<nr  ;  ++i  ) {
					k.rdwr(file);
					uint16 idx;
					file->rdwr_short(idx);
					if(  besch==NULL  ||  idx>=besch->get_field_group()->get_field_class_count()  ) {
						// set class index to 0 if it is out of range
						idx = 0;
					}
					fields.append( field_data_t(k, idx) );
				}
			}
			else {
				// each field only stores location
				for(  uint16 i=0  ;  i<nr  ;  ++i  ) {
					k.rdwr(file);
					fields.append( field_data_t(k, 0) );
				}
			}
		}
	}

	if(file->get_version() >= 99014 && file->get_experimental_version() < 12)
	{
		// Was saving/loading of "target_cities".
		sint32 nr = 0;
		file->rdwr_long(nr);
		sint32 city_index = -1;
		for(int i=0; i < nr; i++)
		{
			file->rdwr_long(city_index);
		}
	}
	
	if(file->get_experimental_version() < 9 && file->get_version() < 110006)
	{
		// Necessary to ensure that the industry density is correct after re-loading a game.
		welt->increase_actual_industry_density(100 / besch->get_gewichtung());
	}

	if(  file->get_version() >= 110005  ) {
		file->rdwr_short(times_expanded);
		// statistics
		for(  int s=0;  s<MAX_FAB_STAT;  ++s  ) {
			for(  int m=0;  m<MAX_MONTH;  ++m  ) {
				file->rdwr_longlong( statistics[m][s] );
			}
		}
		file->rdwr_longlong( weighted_sum_production );
		file->rdwr_longlong( weighted_sum_boost_electric );
		file->rdwr_longlong( weighted_sum_boost_pax );
		file->rdwr_longlong( weighted_sum_boost_mail );
		file->rdwr_longlong( weighted_sum_power );
		file->rdwr_longlong( aggregate_weight );
		file->rdwr_long( delta_slot );
	}
	else if(  file->is_loading()  ) {
		times_expanded = 0;
		init_stats();
		delta_slot = 0;
	}
	arrival_stats_pax.rdwr( file );
	arrival_stats_mail.rdwr( file );

	if(  file->get_version()>=110007  ) {
		file->rdwr_byte(activity_count);
	}
	else if(  file->is_loading()  ) {
		activity_count = 0;
	}

	// save name
	if(  file->get_version() >= 110007  ) {
		if(  file->is_saving() &&  !name  ) {
			char const* fullname = besch->get_name();
			file->rdwr_str(fullname);
		}
		else {
			file->rdwr_str(name);
			if(  file->is_loading()  &&  besch != NULL  &&  name == besch->get_name()  ) {
				// equal to besch name
				name = 0;
			}
		}
	}

	if(file->get_experimental_version() >= 12)
	{
		grund_t *gr = welt->lookup(pos_origin);
		if(!gr)
		{
			gr = welt->lookup_kartenboden(pos_origin.get_2d());
		}
		gebaeude_t *gb = gr->find<gebaeude_t>();
		
		building = gb;
		if (building)
		{
			building->set_fab(this);
		}
	}

	has_calculated_intransit_percentages = false;
	// Cannot calculate intransit percentages here,
	// as this can only be done when paths are available.

}


/**
 * let the chimney smoke, if there is something to produce
 * @author Hj. Malthaner
 */
void fabrik_t::smoke() const
{
	const rauch_besch_t *rada = besch->get_rauch();
	if(rada) {
		const koord size = besch->get_haus()->get_groesse(0)-koord(1,1);
		const uint8 rot = rotate%besch->get_haus()->get_all_layouts();
		koord ro = rada->get_pos_off(size,rot);
		grund_t *gr = welt->lookup_kartenboden(pos.get_2d()+ro);
		// to get same random order on different compilers
		const sint8 offsetx =  ((rada->get_xy_off(rot).x+sim_async_rand(7)-3)*OBJECT_OFFSET_STEPS)/16;
		const sint8 offsety =  ((rada->get_xy_off(rot).y+sim_async_rand(7)-3)*OBJECT_OFFSET_STEPS)/16;
		wolke_t *smoke =  new wolke_t(gr->get_pos(), offsetx, offsety, rada->get_bilder() );
		gr->obj_add(smoke);
		welt->sync_way_eyecandy_add( smoke );
	}
}


uint32 fabrik_t::scale_output_production(const uint32 product, uint32 menge) const
{
	// prorate production based upon amount of product in storage
	// but allow full production rate for storage amounts less than the normal minimum distribution amount (10)
	const uint32 maxi = ausgang[product].max;
	const uint32 actu = ausgang[product].menge;
	if(  actu<maxi  ) {
		if(  actu >= ((10+1)<<fabrik_t::precision_bits)-1  ) {
			if(  menge>(0x7FFFFFFFu/maxi)  ) {
				// avoid overflow
				menge = (((maxi-actu)>>5)*(menge>>5))/(maxi>>10);
			}
			else {
				// and that is the simple formula
				menge = (menge*(maxi-actu)) / maxi;
			}
		}
	}
	else {
		// overfull? No production
		menge = 0;
	}
	return menge;
}


sint32 fabrik_t::input_vorrat_an(const ware_besch_t *typ)
{
	sint32 menge = -1;

	FOR(array_tpl<ware_production_t>, const& i, eingang) {
		if (typ == i.get_typ()) {
			menge = i.menge >> precision_bits;
			break;
		}
	}

	return menge;
}


sint32 fabrik_t::vorrat_an(const ware_besch_t *typ)
{
	sint32 menge = -1;

	FOR(array_tpl<ware_production_t>, const& i, ausgang) {
		if (typ == i.get_typ()) {
			menge = i.menge >> precision_bits;
			break;
		}
	}

	return menge;
}


sint32 fabrik_t::liefere_an(const ware_besch_t *typ, sint32 menge)
{
	if(  typ==warenbauer_t::passagiere  ) {
		// book pax arrival and recalculate pax boost
		book_stat(menge, FAB_PAX_ARRIVED);
		arrival_stats_pax.book_arrival(menge);
		update_prodfactor_pax();
		return menge;
	}
	else if(  typ==warenbauer_t::post  ) {
		// book mail arrival and recalculate mail boost
		book_stat(menge, FAB_MAIL_ARRIVED);
		arrival_stats_mail.book_arrival(menge);
		update_prodfactor_mail();
		return menge;
	}
	else {
		// case : freight
		FOR(  array_tpl<ware_production_t>, & ware, eingang) {
			if(  ware.get_typ() == typ  ) {
				// Can't use update_transit for interface reasons; we don't take a ware argument.
				// We should, however.
				ware.transit -= menge;
				ware.set_stat( ware.transit, FAB_GOODS_TRANSIT );
				// Hajo: avoid overflow
				if(  ware.menge < (FAB_MAX_INPUT - menge) << precision_bits  ) {
					ware.menge += menge << precision_bits;
					ware.book_stat(menge, FAB_GOODS_RECEIVED);
				}
				return menge;
			}
		}
	}
	// ware "typ" wird hier nicht verbraucht
	return -1;
}


sint8 fabrik_t::is_needed(const ware_besch_t *typ) const
{
	/* NOTE for merging with the latest Standard nightlies:
	* this code is changed in the latest Standard nightlies. The
	* idea of the change appears to be to scale the effect
	* of the intransit percentage to the output store of the
	* producing industry. This is not necessary in Experimental,
	* as the max_intransit percentage is in any event scaled
	* based on the lead time and consumption rate. Therefore, the
	* additional code from Standard for this feature should be
	* removed/deleted on merging, and the below original code
	* should remain. 
	*/

	FOR(array_tpl<ware_production_t>, const& i, eingang) {
		if(  i.get_typ() == typ  ) {
			// not needed (false) if overflowing or too much already sent			
			return max_intransit_percentages.get(typ->get_catg()) == 0 ? (i.menge < i.max) : ((i.transit + (i.menge >> fabrik_t::precision_bits)) * 200) < ((i.max >> fabrik_t::precision_bits) * (sint32)max_intransit_percentages.get(typ->get_catg()));
		}
	}
	return -1;  // not needed here
}


bool fabrik_t::is_active_lieferziel( koord k ) const
{
	assert( lieferziele.is_contained(k) );
	return 0 < ( ( 1 << lieferziele.index_of(k) ) & lieferziele_active_last_month );
}



void fabrik_t::step(long delta_t)
{
	if(!has_calculated_intransit_percentages)
	{
		// Can only do it here (once after loading) as paths
		// are not available when loading, even in laden_a....
		calc_max_intransit_percentages();
	}
	
	if(  delta_t==0  ) {
		return;
	}

	// produce nothing/consumes nothing ...
	if(  eingang.empty()  &&  ausgang.empty()  ) {
		// power station? => produce power
		if(  besch->is_electricity_producer()  ) {
			currently_producing = true;
			power = (uint32)( ((sint64)scaled_electric_amount * (sint64)(DEFAULT_PRODUCTION_FACTOR + prodfactor_pax + prodfactor_mail)) >> DEFAULT_PRODUCTION_FACTOR_BITS );
		}

		// produced => trigger smoke
		delta_menge = 1 << fabrik_t::precision_bits;
	}
	else {
		// not a producer => then consume electricity ...
		if(  !besch->is_electricity_producer()  &&  scaled_electric_amount>0  ) {
			// TODO: Consider linking this to actual production only

			prodfactor_electric = (sint32)( ( (sint64)(besch->get_electric_boost()) * (sint64)power + (sint64)(scaled_electric_amount >> 1) ) / (sint64)scaled_electric_amount );

		}

		// calculate the produktion per delta_t; scaled to PRODUCTION_DELTA_T
		// default prodfactor = 256 => shift 8, default time = 1024 => shift 10, rest precision
		const uint64 max_prod = (uint64)prodbase * (uint64)(get_prodfactor());
		const uint64 menge_prod = (max_prod >> (18-10+DEFAULT_PRODUCTION_FACTOR_BITS-fabrik_t::precision_bits)) * (uint64)delta_t + (uint64)menge_remainder;
		const uint32 menge = (uint32)(menge_prod / (uint64)PRODUCTION_DELTA_T);
		menge_remainder = (uint32)(menge_prod - (uint64)menge * (uint64)PRODUCTION_DELTA_T);

		// needed for electricity
		currently_producing = false;
		power_demand = 0;

		if(  ausgang.empty()  ) {
			// consumer only ...
			if(  besch->is_electricity_producer()  ) {
				// power station => start with no production
				power = 0;
			}

			// finally consume stock
			for(  uint32 index = 0;  index < eingang.get_count();  index++  ) {
				if(!besch->get_lieferant(index))
				{
					continue;
				}
				const uint32 vb = besch->get_lieferant(index)->get_verbrauch();
				const uint32 v = max(1,(menge*vb) >> 8);

				if(  (uint32)eingang[index].menge > v + 1  ) {
					eingang[index].menge -= v;
					eingang[index].book_stat(v, FAB_GOODS_CONSUMED);
					currently_producing = true;
					if(  besch->is_electricity_producer()  ) {
						// power station => produce power
						power += (uint32)( ((sint64)scaled_electric_amount * (sint64)(DEFAULT_PRODUCTION_FACTOR + prodfactor_pax + prodfactor_mail)) >> DEFAULT_PRODUCTION_FACTOR_BITS );
					}
					// to find out, if storage changed
					delta_menge += v;
				}
				else {
					if(  besch->is_electricity_producer()  ) {
						// power station => produce power
						power += (uint32)( (((sint64)scaled_electric_amount * (sint64)(DEFAULT_PRODUCTION_FACTOR + prodfactor_pax + prodfactor_mail)) >> DEFAULT_PRODUCTION_FACTOR_BITS) * eingang[index].menge / (v + 1) );
					}
					
					delta_menge += eingang[index].menge;
					eingang[index].book_stat(eingang[index].menge, FAB_GOODS_CONSUMED);
					eingang[index].menge = 0;
				}
			}
		}
		else {
			// ok, calulate maximum allowed consumption
			uint32 min_menge = 0x7FFFFFFF;
			uint32 consumed_menge = 0;
			for(  uint32 index = 0;  index < eingang.get_count();  index++  ) {
				// verbrauch fuer eine Einheit des Produktes (in 1/256)
				if(! besch->get_lieferant(index))
				{
					continue;
				}
				const uint32 vb = besch->get_lieferant(index)->get_verbrauch();
				const uint32 n = eingang[index].menge * 256 / vb;

				if(  n < min_menge  ) {
					min_menge = n;    // finde geringsten vorrat
				}
			}

			// produces something
			for(  uint32 product = 0;  product < ausgang.get_count();  product++  ) {
				uint32 menge_out;

				if(  eingang.get_count() > 0  ) {
					// calculate production
					const uint32 p_menge = scale_output_production( product, menge );
					menge_out = p_menge < min_menge ? p_menge : min_menge;  // production smaller than possible due to consumption
					if(  menge_out > consumed_menge  ) {
						consumed_menge = menge_out;
					}
				}
				else {
					// source producer
					menge_out = scale_output_production( product, menge );
				}

				if(  menge_out > 0  ) {
					const uint32 pb = besch->get_produkt(product)->get_faktor();
					// ensure some minimum production
					const uint32 p = max(1,(menge_out*pb) >> 8);

					// produce
					if(  ausgang[product].menge < ausgang[product].max  ) {
						// to find out, if storage changed
						delta_menge += p;
						ausgang[product].menge += p;
						ausgang[product].book_stat(p, FAB_GOODS_PRODUCED);
						// Consume electricity if the industry is producing anything at all.
						currently_producing = p > 0;
					}
					else {
						ausgang[product].book_stat(ausgang[product].max - 1 - ausgang[product].menge, FAB_GOODS_PRODUCED);
						ausgang[product].menge = ausgang[product].max - 1;
					}
				}
			}

			// and finally consume stock
			for(  uint32 index = 0;  index < eingang.get_count();  index++  ) {
				if(! besch->get_lieferant(index))
				{
					continue;
				}
				const uint32 vb = besch->get_lieferant(index)->get_verbrauch();
				const uint32 v = (consumed_menge*vb) >> 8;

				if(  (uint32)eingang[index].menge > v + 1  ) {
					eingang[index].menge -= v;
					eingang[index].book_stat(v, FAB_GOODS_CONSUMED);
				}
				else {
					eingang[index].book_stat(eingang[index].menge, FAB_GOODS_CONSUMED);
					eingang[index].menge = 0;
				}
			}

		}

		if(  currently_producing || besch->get_produkte() == 0  ) {
			// Pure consumers (i.e., those that do not produce anything) should require full power at all times
			// requires full power even if runs out of raw material next cycle
			power_demand = scaled_electric_amount;
		}
	}

	// increment weighted sums for average statistics
	book_weighted_sums(delta_t);

	// not a power station => then consume all electricity ...
	if(  !besch->is_electricity_producer()  ) {
		power = 0;
	}

	delta_sum += delta_t;
	if(  delta_sum > PRODUCTION_DELTA_T  ) {
		delta_sum = delta_sum % PRODUCTION_DELTA_T;

		// distribute, if there are more than 10 waiting ...
		for(  uint32 produkt = 0;  produkt < ausgang.get_count();  produkt++  ) {
			// either more than ten or nearly full (if there are less than ten output)
			if(  ausgang[produkt].menge > (10 << precision_bits)  ||  ausgang[produkt].menge*2 > ausgang[produkt].max  ) {

				verteile_waren(produkt);
				INT_CHECK("simfab 636");
			}
		}

		recalc_factory_status();

		// rescale delta_menge here: all products should be produced at least once
		// (if consumer only: all supplements should be consumed once)
		const uint32 min_change = ausgang.empty() ? eingang.get_count() : ausgang.get_count();

		if(  (delta_menge>>fabrik_t::precision_bits)>min_change  ) {

			// we produced some real quantity => smoke
			smoke();

			// Knightly : chance to expand every 256 rounds of activities, after which activity count will return to 0 (overflow behaviour)
			if(  ++activity_count==0  ) {
				if(  besch->get_field_group()  ) {
					if(  fields.get_count()<besch->get_field_group()->get_max_fields()  ) {
						// spawn new field with given probability
						add_random_field(besch->get_field_group()->get_probability());
					}
				}
				else {
					if(  times_expanded<besch->get_expand_times()  ) {
						if(  simrand(10000, "fabrik_t::step (expand 1)")<besch->get_expand_probability()  ) {
							set_base_production( prodbase + besch->get_expand_minumum() + simrand( besch->get_expand_range(), "fabrik_t::step (expand 2)" ) );
							++times_expanded;
						}
					}
				}
			}

			INT_CHECK("simfab 558");
			// reset for next cycle
			delta_menge = 0;
		}

	}

	// Knightly : advance arrival slot at calculated interval and recalculate boost where necessary
	delta_slot += delta_t;
	const sint32 periods = welt->get_settings().get_factory_arrival_periods();
	const sint32 slot_interval = (1 << (PERIOD_BITS - SLOT_BITS)) * periods;
	while(  delta_slot>slot_interval  ) {
		delta_slot -= slot_interval;
		const sint32 pax_result = arrival_stats_pax.advance_slot();
		if(  pax_result&ARRIVALS_CHANGED  ||  (periods>1  &&  pax_result&ACTIVE_SLOTS_INCREASED  &&  arrival_stats_pax.get_active_slots()*periods>SLOT_COUNT  )  ) {
			update_prodfactor_pax();
		}
		const sint32 mail_result = arrival_stats_mail.advance_slot();
		if(  mail_result&ARRIVALS_CHANGED  ||  (periods>1  &&  mail_result&ACTIVE_SLOTS_INCREASED  &&  arrival_stats_mail.get_active_slots()*periods>SLOT_COUNT  )  ) {
			update_prodfactor_mail();
		}
	}
}


class distribute_ware_t
{
public:
	ware_t ware;				/// goods to be routed to consumer
	nearby_halt_t nearby_halt;  /// potential start halt
	sint32 space_left;			/// free space at halt
	sint32 amount_waiting;		/// waiting goods at halt for same destination as ware
private:
	sint32 ratio_free_space;	/// ratio of free space at halt (=0 for overflowing station)

public:
	distribute_ware_t( nearby_halt_t n, sint32 l, sint32 t, sint32 a, ware_t w )
	{
		nearby_halt = n;
		space_left = l;
		amount_waiting = a;
		ware = w;
		// Ensure that over-full stations compare equally allowing tie breaker clause (amount waiting)
		sint32 space_total = t > 0 ? t : 1;
		ratio_free_space = space_left > 0 ? ((sint64)space_left << fabrik_t::precision_bits) / space_total : 0;
	}
	distribute_ware_t() {}

	static bool compare(const distribute_ware_t &dw1, const distribute_ware_t &dw2)
	{
		return  (dw1.ratio_free_space > dw2.ratio_free_space)
				||  (dw1.ratio_free_space == dw2.ratio_free_space  &&  dw1.amount_waiting <= dw2.amount_waiting);
	}
};


/**
 * distribute stuff to all best destination
 * @author Hj. Malthaner
 */
void fabrik_t::verteile_waren(const uint32 produkt)
{	
	// wohin liefern ?
	if(  lieferziele.empty()  ) {
		return;
	}

	if(nearby_freight_halts.empty())
	{
		return;
	}

	static vector_tpl<distribute_ware_t> dist_list(16);
	dist_list.clear();

	// to distribute to all target equally, we use this counter, for the source hald, and target factory, to try first
	ausgang[produkt].index_offset++;

	/* prissi: distribute goods to factory
	 * that has not an overflowing input storage
	 * also prevent stops from overflowing, if possible
	 * Since we can called with menge>max/2 are at least 10 are there, we must first limit the amount we distribute
	 */
	sint32 menge = min( (prodbase > 640 ? (prodbase>>6) : 10), ausgang[produkt].menge >> precision_bits );

	const uint32 count = nearby_freight_halts.get_count();
	for(unsigned i = 0; i < count; i++)
	{
		nearby_halt_t nearby_halt = nearby_freight_halts[(i + ausgang[produkt].index_offset) % count];

		// �ber alle Ziele iterieren ("Iterate over all targets" - Google)
		for(  uint32 n=0;  n<lieferziele.get_count();  n++  ) {
			// prissi: this way, the halt that is tried first will change. As a result, if all destinations are empty, it will be spread evenly
			const koord lieferziel = lieferziele[(n + ausgang[produkt].index_offset) % lieferziele.get_count()];
			fabrik_t * ziel_fab = get_fab(lieferziel);

			if(  ziel_fab  ) {
				const sint8 needed = ziel_fab->is_needed(ausgang[produkt].get_typ());
				if(  needed>=0  ) {
					ware_t ware(ausgang[produkt].get_typ(), nearby_halt.halt);
					ware.menge = menge;
					ware.set_zielpos( lieferziel );
					ware.arrival_time = welt->get_zeit_ms();

					unsigned w;
					// find the index in the target factory
					for(  w = 0;  w < ziel_fab->get_eingang().get_count()  &&  ziel_fab->get_eingang()[w].get_typ() != ware.get_besch();  w++  ) {
						// empty
					}

					// if only overflown factories found => deliver to first
					// else deliver to non-overflown factory
					if(  !welt->get_settings().get_just_in_time()  ) {
						// without production stop when target overflowing, distribute to least overflow target
						const sint32 fab_left = ziel_fab->get_eingang()[w].max - ziel_fab->get_eingang()[w].menge;
						dist_list.insert_ordered( distribute_ware_t(nearby_halt, fab_left, ziel_fab->get_eingang()[w].max, (sint32)nearby_halt.halt->get_ware_fuer_zielpos(ausgang[produkt].get_typ(),ware.get_zielpos()), ware ), distribute_ware_t::compare);
					}
					else if(  needed > 0  ) {
						// we are not overflowing: Station can only store up to a maximum amount of goods per square
						const sint32 halt_left = (sint32)nearby_halt.halt->get_capacity(2) - (sint32)nearby_halt.halt->get_ware_summe(ware.get_besch());
						dist_list.insert_ordered( distribute_ware_t(nearby_halt, halt_left, nearby_halt.halt->get_capacity(2), (sint32)nearby_halt.halt->get_ware_fuer_zielpos(ausgang[produkt].get_typ(),ware.get_zielpos()), ware ), distribute_ware_t::compare);
					}
				}
			}
		}
	}

	// Auswertung der Ergebnisse
	// "Evaluation of the results" (Babelfish)
	if(!dist_list.empty())
	{
		distribute_ware_t *best = NULL;
		// Assume a fixed 1km/h transshipment time of goods to industries. This gives a minimum transfer time
		// of 15 minutes for each stop at 125m/tile.
		const uint32 transfer_journey_time_factor = ((uint32)welt->get_settings().get_meters_per_tile() * 6) * 10;
		FOR(vector_tpl<distribute_ware_t>, & i, dist_list) 
		{
			// now search route
			const uint32 transfer_time = ((uint32)i.nearby_halt.distance * transfer_journey_time_factor) / 100;
			const uint32 current_journey_time = (uint32)i.nearby_halt.halt->find_route(i.ware) + transfer_time;
			if(current_journey_time < 65535)
			{
				best = &i;
				break;
			}
		}

		if(  best == NULL  ) {
			return; // no route for any destination
		}

		halthandle_t &best_halt = best->nearby_halt.halt;
		ware_t       &best_ware = best->ware;

		// now process found route
		const sint32 space_left = welt->get_settings().get_just_in_time() ? best->space_left : (sint32)best_halt->get_capacity(2) - (sint32)best_halt->get_ware_summe(best_ware.get_besch());
		menge = min( menge, 9 + space_left );
		// ensure amount is not negative ...
		if(  menge<0  ) {
			menge = 0;
		}
		// since it is assigned here to an unsigned variable!
		best_ware.menge = menge;

		if(  space_left<0  ) {
			// find, what is most waiting here from us
			ware_t most_waiting(ausgang[produkt].get_typ());
			most_waiting.menge = 0;
			FOR(vector_tpl<koord>, const& n, lieferziele) {
				uint32 const amount = best_halt->get_ware_fuer_zielpos(ausgang[produkt].get_typ(), n);
				if(  amount > most_waiting.menge  ) {
					most_waiting.set_zielpos(n);
					most_waiting.menge = amount;
					most_waiting.arrival_time = welt->get_zeit_ms();
				}
			}

			//  we will reroute some goods
			if(  best->amount_waiting==0  &&  most_waiting.menge>0  ) {
				// remove something from the most waiting goods
				if(  best_halt->recall_ware( most_waiting, min((sint32)(most_waiting.menge/2), 1 - space_left) )  ) {
					best_ware.menge += most_waiting.menge;
				}
				else {
					// overcrowded with other stuff (not from us)
					return;
				}
			}
			else {
				// overflowed with our own ware and we have still nearly full stock
				if(  ausgang[produkt].menge>= (3 * ausgang[produkt].max) >> 2  ) {
					/* Station too full, notify player */
					best_halt->bescheid_station_voll();
				}
				return;
			}
		}
		ausgang[produkt].menge -= menge << precision_bits;
		best_halt->starte_mit_route(best_ware);
		best_halt->recalc_status();
		fabrik_t::update_transit( best_ware, true );
		// add as active destination
		lieferziele_active_last_month |= (1 << lieferziele.index_of(best_ware.get_zielpos()));
		ausgang[produkt].book_stat(best_ware.menge, FAB_GOODS_DELIVERED);
	}
}


void fabrik_t::neuer_monat()
{
	// calculate weighted averages
	if(  aggregate_weight>0  ) {
		set_stat( weighted_sum_production / aggregate_weight, FAB_PRODUCTION );
		set_stat( weighted_sum_boost_electric / aggregate_weight, FAB_BOOST_ELECTRIC );
		set_stat( weighted_sum_boost_pax / aggregate_weight, FAB_BOOST_PAX );
		set_stat( weighted_sum_boost_mail / aggregate_weight, FAB_BOOST_MAIL );
		set_stat( weighted_sum_power / aggregate_weight, FAB_POWER );
	}

	// update statistics for input and output goods
	FOR(array_tpl<ware_production_t>, & g, eingang) {
		g.roll_stats(aggregate_weight);
	}
	FOR(array_tpl<ware_production_t>, & g, ausgang) {
		g.roll_stats(aggregate_weight);
	}
	lieferziele_active_last_month = 0;

	// update statistics
	for(  int s=0;  s<MAX_FAB_STAT;  ++s  ) {
		for(  int m=MAX_MONTH-1;  m>0;  --m  ) {
			statistics[m][s] = statistics[m-1][s];
		}
		statistics[0][s] = 0;
	}
	weighted_sum_production = 0;
	weighted_sum_boost_electric = 0;
	weighted_sum_boost_pax = 0;
	weighted_sum_boost_mail = 0;
	weighted_sum_power = 0;
	aggregate_weight = 0;

	// restore the current values
	set_stat( get_current_production(), FAB_PRODUCTION );
	set_stat( prodfactor_electric, FAB_BOOST_ELECTRIC );
	set_stat( prodfactor_pax, FAB_BOOST_PAX );
	set_stat( prodfactor_mail, FAB_BOOST_MAIL );
	set_stat( power, FAB_POWER );

	// since target cities' population may be increased -> re-apportion pax/mail demand
	//recalc_demands_at_target_cities();

	// This needs to be re-checked regularly, as cities grow, occasionally shrink and can be deleted.
	stadt_t* c = welt->get_city(pos.get_2d());

	if(c && !c->get_city_factories().is_contained(this))
	{
		c->add_city_factory(this);
		c->update_city_stats_with_building(get_building(), false);
	}

	if(c != city && city)
	{
		city->remove_city_factory(this);
		city->update_city_stats_with_building(get_building(), true);
	}

	if(!c)
	{
		// Factory no longer in city.
		transformer_connected = NULL;
	}

	city = c;

	mark_connected_roads(false);
	
	calc_max_intransit_percentages();

	// Check to see whether factory is obsolete.
	// If it is, give it a chance of being closed down.
	// @author: jamespetts

	if(welt->use_timeline() && besch->get_haus()->get_retire_year_month() < welt->get_timeline_year_month())
	{
		const uint32 difference =  welt->get_timeline_year_month() - besch->get_haus()->get_retire_year_month();
		const uint32 max_difference =welt->get_settings().get_factory_max_years_obsolete() * 12;
		bool closedown = false;
		if(difference > max_difference)
		{
			closedown = true;
		}
		
		else
		{
			uint32 proportion = (difference * 100) / max_difference;
			proportion *= 75; //Set to percentage value, but take into account fact will be frequently checked (would otherwise be * 100 - reduced to take into account frequency of checking)
			const uint32 chance = (simrand(1000000, "void fabrik_t::neuer_monat()"));
			if(chance <= proportion)
			{
				closedown = true;
			}
		}

		if(closedown)
		{
			char buf[192];
			
			const int upgrades_count = besch->get_upgrades_count();
			if(upgrades_count > 0)
			{
				// This factory has some upgrades: consider upgrading.
				minivec_tpl<const fabrik_besch_t*> upgrade_list(upgrades_count);
				const uint32 max_density = (welt->get_target_industry_density() * 150) / 100;
				const uint32 adjusted_density = welt->get_actual_industry_density() - (100 / besch->get_gewichtung());
				for(uint16 i = 0; i < upgrades_count; i ++)
				{
					// Check whether any upgrades are suitable.
					// Currently, they must be of identical size, as the 
					// upgrade mechanism is quite simple. In future, it might
					// be possible to write more sophisticated upgrading code
					// to enable industries that are not identical in such a
					// way to be upgraded. (Previously, the industry also
					// had to have the same number of suppliers and consumers,
					// but this is no longer necessary given the industry re-linker).

					// Thus, non-suitable upgrades are allowed to be specified
					// in the .dat files for future compatibility.

					const fabrik_besch_t* fab = besch->get_upgrades(i);
					if(	fab != NULL && fab->is_electricity_producer() == besch->is_electricity_producer() &&
						fab->get_haus()->get_b() == besch->get_haus()->get_b() &&
						fab->get_haus()->get_h() == besch->get_haus()->get_h() &&
						fab->get_haus()->get_groesse() == besch->get_haus()->get_groesse() &&
						fab->get_haus()->get_intro_year_month() <= welt->get_timeline_year_month() &&
						fab->get_haus()->get_retire_year_month() >= welt->get_timeline_year_month() &&
						adjusted_density < (max_density + (100 / fab->get_gewichtung())))
					{
						upgrade_list.append_unique(fab);
					}
				}
				
				const uint8 list_count = upgrade_list.get_count();
				if(list_count > 0)
				{
					uint32 total_density = 0;
					ITERATE(upgrade_list, j)
					{
						total_density += (100 / upgrade_list[j]->get_gewichtung());
					}
					const uint32 average_density = total_density / list_count;
					const uint32 probability = 1 / ((100 - ((adjusted_density + average_density) / max_density)) * upgrade_list.get_count()) / 100;
					const uint32 chance = simrand(probability, "void fabrik_t::neuer_monat()");
					if(chance < list_count)
					{
						// All the conditions are met: upgrade.
						const int old_distributionweight = besch->get_gewichtung();
						const fabrik_besch_t* new_type = upgrade_list[chance];
						welt->decrease_actual_industry_density(100 / old_distributionweight);
						uint32 percentage = new_type->get_field_group() ? (new_type->get_field_group()->get_max_fields() * 100) / besch->get_field_group()->get_max_fields() : 0;
						const uint16 adjusted_number_of_fields = percentage ? (fields.get_count() * percentage) / 100 : 0;
						delete_all_fields();
						const char* old_name = get_name();
						besch = new_type;
						const char* new_name = get_name();
						get_building()->calc_bild();
						// Base production is randomised, so is an instance value. Must re-set from the type.
						prodbase = besch->get_produktivitaet() + simrand(besch->get_bereich(), "void fabrik_t::neuer_monat()");
						// Re-add the fields
						for(uint16 i = 0; i < adjusted_number_of_fields; i ++)
						{
							add_random_field(10000u);
						}
						// Re-set the expansion counter: an upgraded factory may expand further.
						times_expanded = 0;
						// Re-calculate production/consumption
						if(besch->get_platzierung() == 2 && city && besch->get_produkte() == 0)
						{
							// City consumer industries set their consumption rates by the relative size of the city
							const weighted_vector_tpl<stadt_t*>& cities = welt->get_staedte();

							sint64 biggest_city_population = 0;
							sint64 smallest_city_population = -1;

							for (weighted_vector_tpl<stadt_t*>::const_iterator i = cities.begin(), end = cities.end(); i != end; ++i)
							{
								stadt_t* const c = *i;
								const sint64 pop = c->get_finance_history_month(0,HIST_CITICENS);
								if(pop > biggest_city_population)
								{
									biggest_city_population = pop;
								}
								else if(pop < smallest_city_population || smallest_city_population == -1)
								{
									smallest_city_population = pop;
								}
							}

							const sint64 this_city_population = city->get_finance_history_month(0,HIST_CITICENS);
							sint32 production;

							if(this_city_population == biggest_city_population)
							{
								production = besch->get_bereich();
							}
							else if(this_city_population == smallest_city_population)
							{
								production = 0;
							}
							else
							{
								const int percentage = (this_city_population - smallest_city_population) * 100 / (biggest_city_population - smallest_city_population);
								production = (besch->get_bereich() * percentage) / 100;
							}
							prodbase = besch->get_produktivitaet() + production;
						}
						else if(besch->get_platzierung() == 2 && !city && besch->get_produkte() == 0)
						{
							prodbase = besch->get_produktivitaet();
						}
						else
						{
							prodbase = besch->get_produktivitaet() + simrand(besch->get_bereich(), "fabrik_t::neuer_monat");
						}
	
						prodbase = prodbase > 0 ? prodbase : 1;

						// create input information
						eingang.resize(besch->get_lieferanten() );
						for(  int g=0;  g<besch->get_lieferanten();  ++g  ) {
							const fabrik_lieferant_besch_t *const input = besch->get_lieferant(g);
							eingang[g].set_typ( input->get_ware() );
						}

						// create output information
						ausgang.resize( besch->get_produkte() );
						for(  uint g=0;  g<besch->get_produkte();  ++g  ) {
							const fabrik_produkt_besch_t *const product = besch->get_produkt(g);
							ausgang[g].set_typ( product->get_ware() );
						}

						recalc_storage_capacities();
						adjust_production_for_fields();
						// Re-calculate electricity conspumption, mail and passenger demand, etc.
						update_scaled_electric_amount();
						update_scaled_pax_demand();
						update_scaled_mail_demand();
						update_prodfactor_pax();
						update_prodfactor_mail();
						welt->increase_actual_industry_density(100 / new_type->get_gewichtung());
						sprintf(buf, translator::translate("Industry:\n%s\nhas been upgraded\nto industry:\n%s."), translator::translate(old_name), translator::translate(new_name));
						welt->get_message()->add_message(buf, pos.get_2d(), message_t::industry, CITY_KI, skinverwaltung_t::neujahrsymbol->get_bild_nr(0));
						return;
					}
				}
			}

			welt->closed_factories_this_month.append(this);
		}
	}
	// NOTE: No code should come after this part, as the closing down code may cause this object to be deleted.
}

// static !
unsigned fabrik_t::status_to_color[5] = {COL_RED, COL_ORANGE, COL_GREEN, COL_YELLOW, COL_WHITE };

#define FL_WARE_NULL           1
#define FL_WARE_ALLENULL       2
#define FL_WARE_LIMIT          4
#define FL_WARE_ALLELIMIT      8
#define FL_WARE_UEBER75        16
#define FL_WARE_ALLEUEBER75    32
#define FL_WARE_FEHLT_WAS      64


/* returns the status of the current factory, as well as output */
void fabrik_t::recalc_factory_status()
{
	unsigned long warenlager;
	char status_ein;
	char status_aus;

	int haltcount = nearby_freight_halts.get_count();

	// set bits for input
	warenlager = 0;
	total_transit = 0;
	status_ein = FL_WARE_ALLELIMIT;
	FOR( array_tpl<ware_production_t>, const& j, eingang ) {
		if(  j.menge >= j.max  ) {
			status_ein |= FL_WARE_LIMIT;
		}
		else {
			status_ein &= ~FL_WARE_ALLELIMIT;
		}
		warenlager += j.menge;
		total_transit += j.transit;
		if(  (j.menge >> fabrik_t::precision_bits) == 0  ) {
			status_ein |= FL_WARE_FEHLT_WAS;
		}

	}
	warenlager >>= fabrik_t::precision_bits;
	if(  warenlager==0  ) {
		status_ein |= FL_WARE_ALLENULL;
	}
	total_input = warenlager;

	// one ware missing, but producing
	if(  status_ein & FL_WARE_FEHLT_WAS  &&  !ausgang.empty()  &&  haltcount > 0  ) {
		status = bad;
		return;
	}

	// set bits for output
	warenlager = 0;
	status_aus = FL_WARE_ALLEUEBER75|FL_WARE_ALLENULL;
	FOR( array_tpl<ware_production_t>, const& j, ausgang ) {
		if(  j.menge > 0  ) {

			status_aus &= ~FL_WARE_ALLENULL;
			if(  j.menge >= 0.75 * j.max  ) {
				status_aus |= FL_WARE_UEBER75;
			}
			else {
				status_aus &= ~FL_WARE_ALLEUEBER75;
			}
			warenlager += j.menge;
			status_aus &= ~FL_WARE_ALLENULL;
		}
		else {
			// menge = 0
			status_aus &= ~FL_WARE_ALLEUEBER75;
		}
	}
	warenlager >>= fabrik_t::precision_bits;
	total_output = warenlager;

	// now calculate status bar
	if(  eingang.empty()  ) {
		// does not consume anything, should just produce

		if(  ausgang.empty()  ) {
			// does also not produce anything
			status = nothing;
		}
		else if(  status_aus&FL_WARE_ALLEUEBER75  ||  status_aus&FL_WARE_UEBER75  ) {
			status = inactive;	// not connected?
			if(haltcount>0) {
				if(status_aus&FL_WARE_ALLEUEBER75) {
					status = bad;	// connect => needs better service
				}
				else {
					status = medium;	// connect => needs better service for at least one product
				}
			}
		}
		else {
			status = good;
		}
	}
	else if(  ausgang.empty()  ) {
		// nothing to produce

		if(status_ein&FL_WARE_ALLELIMIT) {
			// we assume not served
			status = bad;
		}
		else if(status_ein&FL_WARE_LIMIT) {
			// served, but still one at limit
			status = medium;
		}
		else if(status_ein&FL_WARE_ALLENULL) {
			status = inactive;	// assume not served
			if(haltcount>0) {
				// there is a halt => needs better service
				status = bad;
			}
		}
		else {
			status = good;
		}
	}
	else {
		// produces and consumes
		if((status_ein&FL_WARE_ALLELIMIT)!=0  &&  (status_aus&FL_WARE_ALLEUEBER75)!=0) {
			status = bad;
		}
		else if((status_ein&FL_WARE_ALLELIMIT)!=0  ||  (status_aus&FL_WARE_ALLEUEBER75)!=0) {
			status = medium;
		}
		else if((status_ein&FL_WARE_ALLENULL)!=0  &&  (status_aus&FL_WARE_ALLENULL)!=0) {
			// not producing
			status = inactive;
		}
		else if(haltcount>0  &&  ((status_ein&FL_WARE_ALLENULL)!=0  ||  (status_aus&FL_WARE_ALLENULL)!=0)) {
			// not producing but out of supply
			status = medium;
		}
		else {
			status = good;
		}
	}
}


void fabrik_t::zeige_info()
{
	create_win(new fabrik_info_t(this, get_building()), w_info, (ptrdiff_t)this );
}


void fabrik_t::info_prod(cbuffer_t& buf) const
{
	buf.clear();
	buf.append( translator::translate("Durchsatz") );
	buf.append( get_current_production(), 0 );
	buf.append( translator::translate("units/day") );
	buf.append( "\n" );
	if(get_besch()->is_electricity_producer())
	{
		buf.append(translator::translate("Electrical output: "));
	}
	else
	{
		buf.append(translator::translate("Electrical demand: "));
	}

	buf.append(scaled_electric_amount>>POWER_TO_MW);
	buf.append(" MW");

	if(city != NULL)
	{
		buf.append("\n\n");
		buf.append(translator::translate("City"));
		buf.append(": ");
		buf.append(city->get_name());
	}
	buf.append("\n");

	if (!ausgang.empty()) {
		buf.append("\n\n");
		buf.append(translator::translate("Produktion"));

		for (uint32 index = 0; index < ausgang.get_count(); index++) {
			const ware_besch_t * type = ausgang[index].get_typ();

			buf.printf( "\n - %s %u/%u%s",
				translator::translate(type->get_name()),
				(sint32)(0.5+ausgang[index].menge / (double)(1<<fabrik_t::precision_bits)),
				(sint32)(ausgang[index].max >> fabrik_t::precision_bits),
				translator::translate(type->get_mass())
			);

			if(type->get_catg() != 0) {
				buf.append(", ");
				buf.append(translator::translate(type->get_catg_name()));
			}

			buf.append(", ");
			buf.append((besch->get_produkt(index)->get_faktor()*100l)/256.0,0);
			buf.append("%");
		}
	}

	if (!eingang.empty()) {
		buf.append("\n\n");
		buf.append(translator::translate("Verbrauch"));

		for (uint32 index = 0; index < eingang.get_count(); index++) {
			if(!besch->get_lieferant(index))
			{
				continue;
			}
			const uint16 max_intransit_percentage = max_intransit_percentages.get(eingang[index].get_typ()->get_catg());

			if(  max_intransit_percentage  ) {
				buf.printf("\n - %s %u/%i(%i)/%u%s, %u%%",
					translator::translate(eingang[index].get_typ()->get_name()),
					(sint32)(0.5+eingang[index].menge / (double)(1<<fabrik_t::precision_bits)),
					eingang[index].transit,
					eingang[index].max_transit,
					(eingang[index].max >> fabrik_t::precision_bits),
					translator::translate(eingang[index].get_typ()->get_mass()),
					(sint32)(0.5+(besch->get_lieferant(index)->get_verbrauch()*100l)/256.0)
				);
			}
			else {
				buf.printf("\n - %s %u/%i/%u%s, %u%%",
					translator::translate(eingang[index].get_typ()->get_name()),
					(sint32)(0.5+eingang[index].menge / (double)(1<<fabrik_t::precision_bits)),
					eingang[index].transit,
					(eingang[index].max >> fabrik_t::precision_bits),
					translator::translate(eingang[index].get_typ()->get_mass()),
					(sint32)(0.5+(besch->get_lieferant(index)->get_verbrauch()*100l)/256.0)
				);
			}
		}
	}
}

/**
 * Recalculate the nearby_freight_halts and nearby_passenger_halts lists.
 * This is a subroutine in order to avoid code duplication.
 * @author neroden
 */
void fabrik_t::recalc_nearby_halts() {
	// Temporary list for accumulation of halts;
	// avoid duplicating work on freight and passenger
	vector_tpl<nearby_halt_t> nearby_halts;

	// Go through all the base tiles of the factory.
	vector_tpl<koord> tile_list;
	get_tile_list(tile_list);
	bool any_distribution_target = false; // just for debugging
	FOR(vector_tpl<koord>, const k, tile_list)
	{
		const planquadrat_t* plan = welt->access(k);
		if(plan)
		{
			any_distribution_target=true;
			const uint8 haltlist_count = plan->get_haltlist_count();
			if(haltlist_count)
			{
				const nearby_halt_t *haltlist = plan->get_haltlist();
				for(int i = 0; i < haltlist_count; i++)
				{
					// We've found a halt.
					const nearby_halt_t new_nearby_halt = haltlist[i];
					// However, it might be a duplicate.
					bool duplicate = false;
					for(uint32 j=0; j < nearby_halts.get_count(); j++)
					{
						if (new_nearby_halt.halt == nearby_halts[j].halt) {
							duplicate=true;
							// Same halt handle.
							// We always want the shorter of the two distances...
							// Since goods/passengers can ship from any part of a factory
							uint8 new_distance = min(nearby_halts[j].distance, new_nearby_halt.distance);
							nearby_halts[j].distance = new_distance;
						}
					}
					if (!duplicate) {
						nearby_halts.append(new_nearby_halt);
					}
				}
			}
		}
	}
#ifdef DEBUG
	if(!any_distribution_target)
	{
		dbg->fatal("fabrik_t::recalc_nearby_halts", "%s has no location on the map!", get_name() );
	}
#endif // DEBUG
	// We now have a list of nearby halts, without duplicates,
	// and each with the shortest distance to it.

	// Clear out the old lists.
	nearby_freight_halts.clear();
	nearby_passenger_halts.clear();
	nearby_mail_halts.clear();

	// Now filter the new list by freight vs. passengers.
	FOR(vector_tpl<nearby_halt_t>, const k, nearby_halts)
	{
		if (k.halt->get_pax_enabled())
		{
			nearby_passenger_halts.append(k);
		}
		if (k.halt->get_post_enabled())
		{
			nearby_mail_halts.append(k);
		}
		// Horribly, we must only recognize freight halts which are within a certain "square" distance
		// of the target halt, thanks to James's computation-intensive "freight coverage" rule.
		// We rely on the meat of halt:verbinde_fabriken having been run already, so that this list of
		// factories is already present in the target halt.
		// It's a list of factory pointers, so we're looking for "this" in it.
		// Horrible horrible pointer comparison dependency...
		if(  k.halt->get_ware_enabled()
			 && k.halt->get_fab_list().is_contained(this) )
		{
			// Halt is within freight coverage distance (shorter than regular) and handles freight...
			if (get_besch()->get_platzierung() == fabrik_besch_t::Wasser
				&& (k.halt->get_station_type() & haltestelle_t::dock) == 0)
			{
				// But this is a water factory and it's not a dock.
				// So do nothing.
			}
			else
			{
				// OK, add to list of freight halts.
				nearby_freight_halts.append(k);
			}
		}
	}
}

void fabrik_t::info_conn(cbuffer_t& buf) const
{
	buf.clear();
	bool has_previous = false;
	if (!lieferziele.empty()) {
		has_previous = true;
		buf.append(translator::translate("Abnehmer"));

		FOR(vector_tpl<koord>, const& lieferziel, lieferziele) {
			fabrik_t *fab = get_fab( lieferziel );
			if(fab) {
				if(  is_active_lieferziel(lieferziel)  ) {
					buf.printf("\n      %s (%d,%d)", translator::translate(fab->get_name()), lieferziel.x, lieferziel.y);
				}
				else {
					buf.printf("\n   %s (%d,%d)", translator::translate(fab->get_name()), lieferziel.x, lieferziel.y);
				}
			}
		}
	}

	if (!suppliers.empty()) {
		if(  has_previous  ) {
			buf.append("\n\n");
		}
		has_previous = true;
		buf.append(translator::translate("Suppliers"));

		FOR(vector_tpl<koord>, const& supplier, suppliers) {
			if(  fabrik_t *src = get_fab( supplier )  ) {
				if(  src->is_active_lieferziel(get_pos().get_2d())  ) {
					buf.printf("\n      %s (%d,%d)", translator::translate(src->get_name()), supplier.x, supplier.y);
				}
				else {
					buf.printf("\n   %s (%d,%d)", translator::translate(src->get_name()), supplier.x, supplier.y);
				}
			}
		}
	}

	//if (!target_cities.empty()) {
	//	if(  has_previous  ) {
	//		buf.append("\n\n");
	//	}
	//	has_previous = true;
	//	buf.append( is_end_consumer() ? translator::translate("Customers live in:") : translator::translate("Arbeiter aus:") );

	//	for(  uint32 c=0;  c<target_cities.get_count();  ++c  ) {
	//		buf.append("\n");
	//	}
	//}

	// Check *all* tiles for nearby stops... but don't update!
	if ( !nearby_freight_halts.empty() )
	{
		if(  has_previous  ) {
			buf.append("\n\n");
		}
		has_previous = true;
		buf.append(translator::translate("Connected stops (freight)"));
		FOR(vector_tpl<nearby_halt_t>, const i, nearby_freight_halts)
		{
			buf.printf("\n - %s", i.halt->get_name());
		}
	}
}


void fabrik_t::laden_abschliessen()
{
	city = welt->get_city(pos.get_2d());
	if(city != NULL)
	{
		city->add_city_factory(this);
		city->update_city_stats_with_building(get_building(), false);
	}
	
	// adjust production base to be at least as large as fields productivity
	uint32 prodbase_adjust = 1;
	const field_group_besch_t *fb = besch->get_field_group();
	if(fb) {
		for(uint32 i=0; i<fields.get_count(); i++) {
			const field_class_besch_t *fc = fb->get_field_class( fields[i].field_class_index );
			if (fc) {
				prodbase_adjust += fc->get_field_production();
			}
		}
	}

	// set production, update all production related numbers
	set_base_production( max(prodbase, prodbase_adjust) );

	// now we have a valid storage limit
	if (welt->get_settings().is_crossconnect_factories()) {
		add_all_suppliers();
	}
	else {
		// add as supplier to target(s)
		for(uint32 i=0; i<lieferziele.get_count(); i++) {
			fabrik_t * fab2 = fabrik_t::get_fab(lieferziele[i]);
			if(fab2) {
				fab2->add_supplier(pos.get_2d());
				lieferziele[i] = fab2->get_pos().get_2d();
			}
			else {
				// remove this ...
				dbg->warning( "fabrik_t::laden_abschliessen()", "No factory at expected position %s!", lieferziele[i].get_str() );
				lieferziele.remove_at(i);
				i--;
			}
		}
	}

	// adjust production base to be at least as large as fields productivity
	adjust_production_for_fields();

	mark_connected_roads(false);
	add_to_world_list();
}

void fabrik_t::adjust_production_for_fields()
{
	uint32 prodbase_adjust = 1;
	const field_group_besch_t *fb = besch->get_field_group();
	if(fb) {
		for(uint32 i=0; i<fields.get_count(); i++) {
			const field_class_besch_t *fc = fb->get_field_class( fields[i].field_class_index );
			if (fc) {
				prodbase_adjust += fc->get_field_production();
			}
		}
	}
	// set production, update all production related numbers
	set_base_production( max(prodbase, prodbase_adjust) );
}

void fabrik_t::rotate90( const sint16 y_size )
{
	rotate = (rotate+3)%besch->get_haus()->get_all_layouts();
	pos_origin.rotate90( y_size );
	pos_origin.x -= besch->get_haus()->get_b(rotate)-1;
	pos.rotate90( y_size );

	FOR(vector_tpl<koord>, & i, lieferziele) {
		i.rotate90(y_size);
	}
	FOR(vector_tpl<koord>, & i, suppliers) {
		i.rotate90(y_size);
	}
	FOR(vector_tpl<field_data_t>, & i, fields) {
		i.location.rotate90(y_size);
	}
}


void fabrik_t::add_supplier(koord ziel)
{
	if(  welt->get_settings().get_factory_maximum_intransit_percentage()  &&  !suppliers.is_contained(ziel)  ) {
		if(  fabrik_t *fab = get_fab( ziel )  ) {
			for(  uint32 i=0;  i < fab->get_ausgang().get_count();  i++   ) {
				const ware_production_t &w_out = fab->get_ausgang()[i];
				// now update transit limits
				FOR(  array_tpl<ware_production_t>,  &w,  eingang ) {
					if(  w_out.get_typ() == w.get_typ()  ) {
#ifdef TRANSIT_DISTANCE
						sint64 distance = koord_distance( fab->get_pos(), get_pos() );
						// calculate next mean by the following formula: average + (next - average)/(n+1)
						w.count_suppliers ++;
						sint64 next = 1 + ( distance * welt->get_settings().get_factory_maximum_intransit_percentage() * (w.max >> fabrik_t::precision_bits) ) / 131072;
						w.max_transit += (next - w.max_transit)/(w.count_suppliers);
#else
						sint32 max_storage = 1 + ( (w_out.max * welt->get_settings().get_factory_maximum_intransit_percentage() ) >> fabrik_t::precision_bits) / 100;
						w.max_transit += max_storage;
#endif
						break;
					}
				}
			}
			// since there could be more than one good, we have to iterate over all of them
		}
	}
	suppliers.insert_unique_ordered( ziel, RelativeDistanceOrdering(pos.get_2d()) );
}


void fabrik_t::rem_supplier(koord pos)
{
	suppliers.remove(pos);

	if(  welt->get_settings().get_factory_maximum_intransit_percentage()  ) {
		// set to zero
		FOR(  array_tpl<ware_production_t>,  &w,  eingang ) {
			w.max_transit = 0;
		}

		// unfourtunately we have to bite the bullet and recalc the values from scratch ...
		FOR( vector_tpl<koord>, ziel, suppliers ) {
			if(  fabrik_t *fab = get_fab( ziel )  ) {
				for(  uint32 i=0;  i < fab->get_ausgang().get_count();  i++   ) {
					const ware_production_t &w_out = fab->get_ausgang()[i];
					// now update transit limits
					FOR(  array_tpl<ware_production_t>,  &w,  eingang ) {
						if(  w_out.get_typ() == w.get_typ()  ) {
#ifdef TRANSIT_DISTANCE
							sint64 distance = koord_distance( fab->get_pos(), get_pos() );
							// calculate next mean by the following formula: average + (next - average)/(n+1)
							w.count_suppliers ++;
							sint64 next = 1 + ( distance * welt->get_settings().get_factory_maximum_intransit_percentage() * (w.max >> fabrik_t::precision_bits) ) / 131072;
							w.max_transit += (next - w.max_transit)/(w.count_suppliers);
#else
							sint32 max_storage = 1 + ( (w_out.max * welt->get_settings().get_factory_maximum_intransit_percentage() ) >> fabrik_t::precision_bits) / 100;
							w.max_transit += max_storage;
#endif
							break;
						}
					}
				}
				// since there could be more than one good, we have to iterate over all of them
			}
		}
	}
}


/** crossconnect everything possible */
void fabrik_t::add_all_suppliers()
{
	lieferziele.clear();
	suppliers.clear();
	for(int i=0; i < besch->get_lieferanten(); i++) {
		const fabrik_lieferant_besch_t *lieferant = besch->get_lieferant(i);
		const ware_besch_t *ware = lieferant->get_ware();

		FOR(vector_tpl<fabrik_t*>, const fab, welt->get_fab_list()) {
			// connect to an existing one, if this is an producer
			if(fab!=this  &&  fab->vorrat_an(ware) > -1) {
				// add us to this factory
				fab->add_lieferziel(pos.get_2d());
				// and vice versa
				add_supplier(fab->get_pos().get_2d());
			}
		}
	}
}


/* adds a new supplier to this factory
 * fails if no matching goods are there
 */
bool fabrik_t::add_supplier(fabrik_t* fab)
{
	for(int i=0; i < besch->get_lieferanten(); i++) {
		const fabrik_lieferant_besch_t *lieferant = besch->get_lieferant(i);
		const ware_besch_t *ware = lieferant->get_ware();

			// connect to an existing one, if this is an producer
			if(  fab!=this  &&  fab->vorrat_an(ware) > -1  ) { //"inventory to" (Google)
				// add us to this factory
				fab->add_lieferziel(pos.get_2d());
				return true;
			}
	}
	return false;
}

/* adds a new customer to this factory
 * fails if no matching goods are accepted
 */

bool fabrik_t::add_customer(fabrik_t* fab)
{
	for(int i=0; i < fab->get_besch()->get_lieferanten(); i++) {
		const fabrik_lieferant_besch_t *lieferant = fab->get_besch()->get_lieferant(i);
		const ware_besch_t *ware = lieferant->get_ware();

			// connect to an existing one, if it is a consumer
			if(fab!=this  &&  vorrat_an(ware) > -1) { //"inventory to" (Google)
				// add this factory
				add_lieferziel(fab->pos.get_2d());
				return true;
			}
	}
	return false;
}

void fabrik_t::get_tile_list( vector_tpl<koord> &tile_list ) const
{
	tile_list.clear();

	koord pos_2d = pos.get_2d();
	const fabrik_besch_t* besch = get_besch();
	if(!besch)
	{
		return;
	}
	const haus_besch_t* haus_besch = besch->get_haus();
	if(!haus_besch)
	{
		return;
	}
	koord size = haus_besch->get_groesse(this->get_rotate());
	koord test;
	// Which tiles belong to the fab?
	for( test.x = 0; test.x < size.x; test.x++ ) {
		for( test.y = 0; test.y < size.y; test.y++ ) {
			if( fabrik_t::get_fab( pos_2d+test ) == this ) {
				tile_list.append( pos_2d+test );
			}
		}
	}
}

// Returns a list of goods produced by this factory. The caller must delete
// the list when done
slist_tpl<const ware_besch_t*> *fabrik_t::get_produced_goods() const
{
	slist_tpl<const ware_besch_t*> *goods = new slist_tpl<const ware_besch_t*>();

	FOR(array_tpl<ware_production_t>, const& i, ausgang) {
		goods->append(i.get_typ());
	}

	return goods;
}


void fabrik_t::add_to_world_list()
{
	welt->add_building_to_world_list(get_building());
}

gebaeude_t* fabrik_t::get_building()
{
	if(building)
	{
		return building;
	}
	const grund_t* gr = welt->lookup(pos);
	if(gr)
	{
		building = gr->find<gebaeude_t>();
		return building;
	}
	return NULL;
}


void fabrik_t::calc_max_intransit_percentages()
{
	max_intransit_percentages.clear();

	if(!path_explorer_t::get_paths_available(path_explorer_t::get_current_compartment()))
	{
		has_calculated_intransit_percentages = false;
		return;
	}

	has_calculated_intransit_percentages = true;
	const uint16 base_max_intransit_percentage = welt->get_settings().get_factory_maximum_intransit_percentage();
	
	uint32 index = 0;
	FOR(array_tpl<ware_production_t>, &w, eingang) 
	{
		const uint8 catg = w.get_typ()->get_catg();
		if(base_max_intransit_percentage == 0)
		{
			// Zero is code for the feature being disabled, so do not attempt to modify this value.
			max_intransit_percentages.put(catg, base_max_intransit_percentage);
			index ++;
			continue;
		}

		const uint16 lead_time = get_lead_time(w.get_typ());
		if(lead_time == 65535)
		{
			// No factories connected; use the default intransit percentage for now.
			max_intransit_percentages.put(catg, base_max_intransit_percentage);
			index ++;
			continue;
		}
		const uint16 time_to_consume = max(1, get_time_to_consume_stock(index)); 
		const uint32 ratio = ((uint32)lead_time * 1000 / (uint32)time_to_consume);
		const uint32 modified_max_intransit_percentage = (ratio * (uint32)base_max_intransit_percentage) / 1000;
		max_intransit_percentages.put(catg, (uint16)modified_max_intransit_percentage);
		index ++;
	}
}

uint32 fabrik_t::get_lead_time(const ware_besch_t* wtype)
{
	if(suppliers.empty())
	{
		return 65535;
	}
	
	// Tenths of minutes.
	uint32 longest_lead_time = 65535;

	FOR(vector_tpl<koord>, const& supplier, suppliers)
	{
		const fabrik_t *fab = get_fab(supplier);
		if(!fab)
		{
			continue;
		}
		for (uint i = 0; i < fab->get_besch()->get_produkte(); i++) 
		{
			const fabrik_produkt_besch_t *product = fab->get_besch()->get_produkt(i);
			if(product->get_ware() == wtype)
			{
				uint16 best_journey_time = 65535;
				const uint32 transfer_journey_time_factor = ((uint32)welt->get_settings().get_meters_per_tile() * 6) * 10;

				FOR(vector_tpl<nearby_halt_t>, const& nearby_halt, fab->nearby_freight_halts) 
				{
					// now search route
					const uint32 transfer_time = ((uint32)nearby_halt.distance * transfer_journey_time_factor) / 100;
					ware_t tmp;
					tmp.set_besch(wtype);
					tmp.set_zielpos(pos.get_2d());
					tmp.set_origin(nearby_halt.halt);
					const uint32 current_journey_time = (uint32)nearby_halt.halt->find_route(tmp, best_journey_time) + transfer_time;
					if(current_journey_time < best_journey_time)
					{
						best_journey_time = current_journey_time;
					}
				}

				if(best_journey_time < 65535 && (best_journey_time > longest_lead_time || longest_lead_time == 65535))
				{
					longest_lead_time = best_journey_time;
				}
				break;
			}
		}
		
	}

	return longest_lead_time;
}

uint32 fabrik_t::get_time_to_consume_stock(uint32 index)
{
	// This should work in principle, but as things currently stand, 
	// rounding errors result in monthly consumption that is too high
	// in some cases (especially where the base production figure is low).
	const fabrik_lieferant_besch_t* flb = besch->get_lieferant(index);
	const uint32 vb = flb ? flb->get_verbrauch() : 0;
	const sint32 base_production = get_current_production();
	const sint32 consumed_per_month = vb == 0 ? 1 : (base_production * vb) >> 8;

	const sint32 input_capacity = (eingang[index].max >> fabrik_t::precision_bits);

	const sint64 tick_units = input_capacity * welt->ticks_per_world_month;

	const sint32 ticks_to_consume = tick_units / max(1, consumed_per_month);
	return welt->ticks_to_tenths_of_minutes(ticks_to_consume);

	/*

	100 ticks per month
	20 units per month
	60 minutes per month
	600 10ths of a minute per month
	storage of 40 units

	40 units * 100 ticks = 4,000 tick units
	4,000 tick units / 20 units per month = 200 ticks

	*/
}

