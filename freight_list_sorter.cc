#include <algorithm>

#include "freight_list_sorter.h"
#include "simhalt.h"
#include "simtypes.h"
#include "simware.h"
#include "simfab.h"
#include "simworld.h"
#include "simcity.h"

#include "dataobj/translator.h"

#include "tpl/slist_tpl.h"
#include "tpl/vector_tpl.h"

#include "utils/cbuffer_t.h"

// Necessary for MinGW
#if !defined(__APPLE__) && !defined(__OpenBSD__)
#include "malloc.h"
#endif

karte_ptr_t freight_list_sorter_t::welt;
freight_list_sorter_t::sort_mode_t freight_list_sorter_t::sortby=by_name;

/**
 *  @return whether w1 is less than w2
 */
bool freight_list_sorter_t::compare_ware(ware_t const& w1, ware_t const& w2)
{
	// sort according to freight
	// if w1 and w2 differ, they are sorted according to catg_index and index
	// we sort with respect to catg_indexfirst, since freights with the same category
	// will be displayed together
	int idx = w1.get_desc()->get_catg_index() - w2.get_desc()->get_catg_index();
	if(  idx == 0  ) {
		idx = w1.get_desc()->get_index() - w2.get_desc()->get_index();
	}
	if(  idx != 0  ) {
		return idx < 0;
	}

	switch (sortby) {
		default:
			dbg->error("freight_list_sorter::compare_ware()", "illegal sort mode!");
			break;

		case by_via_sum:
		case by_amount: { // sort by ware amount
			int const order = w2.menge - w1.menge;
			if(  order != 0  ) {
				return order < 0;
			}
			/* FALLTHROUGH */
		}
		// no break

		case by_via:  // sort by via_destination name
		case by_accommodation_via: // Initial sorting is done already in simconvoi.cc
		{
			halthandle_t const v1 = w1.get_zwischenziel();
			halthandle_t const v2 = w2.get_zwischenziel();
			if(  v1.is_bound() && v2.is_bound()  ) {
				int const order = strcmp(v1->get_name(), v2->get_name());
				if(  order != 0) return order < 0;
			}
			else if(  v1.is_bound()  ) {
				return false;
			}
			else if(  v2.is_bound()  ) {
				return true;
			}
			/* FALLTHROUGH */
		}
		// no break

		case by_origin: // Sort by origin stop name
		case by_origin_amount: {
			halthandle_t const o1 = w1.get_origin();
			halthandle_t const o2 = w2.get_origin();
			if (o1.is_bound() && o2.is_bound()) {
				int const order = strcmp(o1->get_name(), o2->get_name()) < 0;
				if (order != 0) return order < 0;
			} else if (o1.is_bound()) {
				return false;
			} else if (o2.is_bound()) {
				return true;
			}
			/* FALLTHROUGH */
		}
		// no break

		case by_name: { // sort by destination stop name
			halthandle_t const d1 = w1.get_ziel();
			halthandle_t const d2 = w2.get_ziel();
			if(  d1.is_bound()  &&  d2.is_bound()  ) {
				const fabrik_t *fab = NULL;
				const char *const name1 = d1->get_name();
				const char *const name2 = d2->get_name();
				return strcmp(name1, name2) < 0;
			}
			else if (d1.is_bound()) {
				return false;
			}
			else if (d2.is_bound()) {
				return true;
			}
		}

		case by_destination_detail: // Sort by ultimate destination name
		case by_accommodation_detail: // Initial sorting is done already in simconvoi.cc
		{
				
			grund_t* gr = welt->lookup_kartenboden(w1.get_zielpos());
			const gebaeude_t* const gb1 = gr ? gr->find<gebaeude_t>() : NULL;
			gr = welt->lookup_kartenboden(w2.get_zielpos());
			const gebaeude_t* const gb2 = gr ? gr->find<gebaeude_t>() : NULL;
			if(gb1 && gb2)
			{
				const fabrik_t *fab = NULL;
				// TODO -oBG, 29.12.2013: optimize:
				const char *const name1 = (fabrik_t::get_fab(w1.get_zielpos()) && sortby != by_origin ? ((fab = fabrik_t::get_fab(w1.get_zielpos())) ? fab->get_name() : "Invalid Factory" ) : translator::translate(gb1->get_tile()->get_desc()->get_name()));
				const char *const name2 = (fabrik_t::get_fab(w2.get_zielpos()) && sortby != by_origin ? ((fab = fabrik_t::get_fab(w2.get_zielpos())) ? fab->get_name() : "Invalid Factory" ) : translator::translate(gb2->get_tile()->get_desc()->get_name()));
				return strcmp(name1, name2) < 0;
			}
			if(gb1) 
			{
				return false;
			}
			if(gb2)
			{
				return true;
			}
		}
		case by_wealth_detail: { // sort by class
			int const order = w2.get_class() - w1.get_class();
			if (order != 0) {
				return order < 0;
			}
			/* FALLTHROUGH */
		}
		case by_wealth_via: { // sort by class
			halthandle_t const v1 = w1.get_zwischenziel();
			halthandle_t const v2 = w2.get_zwischenziel();
			if (v1.is_bound() && v2.is_bound())
			{
				int const class_order = w2.get_class() - w1.get_class();
				if (class_order == 0)
				{
					int const via_order = strcmp(v1->get_name(), v2->get_name());
					if (via_order != 0)
					{
						return via_order < 0;
					}
				}
				else
				{
					return class_order < 0;
				}
			}
			else if (v1.is_bound()) {
				return false;
			}
			else if (v2.is_bound()) {
				return true;
			}
			/* FALLTHROUGH */
		}
		/*case by_transfer_time: // Should only be used with transfer goods
		{
			const sint64 current_time = welt->get_ticks();

			transferring_cargo_t tc1;
			transferring_cargo_t tc2;
			tc1.ware = w1;
			tc2.ware = w2;
			const uint32 rs1 = world()->ticks_to_seconds((tc1.ready_time - current_time));
			const uint32 rs2 = world()->ticks_to_seconds((tc2.ready_time - current_time));

		
			int const order = rs2 - rs1;
			if (order != 0)
			{
				return order < 0;
			}
		}*/
		// no break
	}
	return false;
}



void freight_list_sorter_t::add_ware_heading(cbuffer_t &buf, uint32 sum, uint32 max, const ware_t *ware, const char *what_doing, uint8 g_class, uint32 total_pass_mail, bool show_empty)
{
	goods_desc_t const& desc = *ware->get_desc();
	char const*  const  name = translator::translate(ware->get_catg() != 0 ? desc.get_catg_name() : desc.get_name());
	char const*  const  what = translator::translate(what_doing);
	char class_entry[32] = "\0";
	char const*  unit = translator::translate(desc.get_mass());
	bool sorting_by_wealth = sortby == by_wealth_detail || sortby == by_wealth_via ? true : false;
	bool sorting_by_accommodation = sortby == by_accommodation_detail || sortby == by_accommodation_via ? true : false;
	bool is_class_cargo = ware->get_index() == goods_manager_t::INDEX_PAS || ware->get_index() == goods_manager_t::INDEX_MAIL ? true : false;

	if (buf.len() > 0) {
		buf.append("\n");
	}
	if (is_class_cargo && sorting_by_wealth && max > 0)
	{// When sorting by wealth, there would be no maximum capacity per "wealth" entry, therefore we need to show one extra entry with maximum capacity for the pass or mail.
		if (total_pass_mail > 0 || sum == 0)
		{// This is apperently the first entry of either pass or mail, and therefore needs the maximum freight indicator.
			buf.printf(" %u/%u", total_pass_mail, max);
			if (ware->get_index() == goods_manager_t::INDEX_PAS)
			{
				buf.printf(" %s %s\n", name, what);
			}
			else 
			{
				buf.printf(" %s %s %s\n", unit, name, what);
			}
		}
		if (sum > 0)
		{// No, this is not the first entry, only show the amount
			buf.printf(" %u", sum);
		}
	}
	else
	{// This is either a non-class cargo or this is a list of freight in a station window.
		buf.printf(" %u", sum);
		if (max != 0) {
			// convois
			buf.printf("/%u", max);
		}
	}

	if (is_class_cargo && (sorting_by_wealth || sorting_by_accommodation))
	{
		char class_name[32];
		sprintf(class_name, ware->get_index() == goods_manager_t::INDEX_PAS ? "p_class[%u]" : "m_class[%u]", g_class);
		sprintf(class_entry, "(%s) ", translator::translate(class_name));
	}
	// Ensure consistent spacing
	if (sorting_by_wealth && sum>0 || !sorting_by_wealth || !is_class_cargo)
	{// Ensure that, if we sort by wealth, we only show this section below if there actually is something to show, alternatively if this is not a freight type with classes at all.
		if (ware->get_index() == goods_manager_t::INDEX_PAS)
		{
			buf.printf(" %s %s%s\n", name, class_entry, what);
		}
		else
		{
			// special freight (catg == 0) needs own name
			buf.printf(" %s %s%s %s\n", unit, class_entry, name, what);
		}
	}
	if (sum == 0 && show_empty)
	{
		buf.printf("   %s\n", translator::translate("leer"));
	}
}



void freight_list_sorter_t::sort_freight(vector_tpl<ware_t> const& warray, cbuffer_t& buf, sort_mode_t sort_mode, const slist_tpl<ware_t>* full_list, const char* what_doing, const uint8 accommodation, const uint32 accommodation_capacity , const ware_t *accommodation_ware, const bool show_empty)
{
	sortby = sort_mode;

	// hsiegeln
	// added sorting to ware's destination list
	int pos = 0;
	ware_t* wlist;
	//ware_t* tclist;
	const int warray_size = warray.get_count() * sizeof(ware_t);
#ifdef _MSC_VER
	const int max_stack_size = 838860; 
	if(warray_size < max_stack_size)
	{
		// Old method - use the stack, but will cause stack overflows if
		// warray_size is too large
		wlist = (ware_t*) alloca(warray_size);
	}
	else
#endif
	{
		// Too large for the stack - use the heap (much slower)
		wlist = (ware_t*) malloc(warray_size);
	}


	FOR(vector_tpl<ware_t>, const& ware, warray) {
		if(  ware.get_desc() == goods_manager_t::none  ||  ware.menge == 0  ) {
			continue;
		}
		wlist[pos] = ware;
		// for the sorting via the number for the next stop we unify entries
		if((sort_mode == by_via_sum || sort_mode == by_accommodation_via) && pos > 0)
		{
			//DBG_MESSAGE("freight_list_sorter_t::get_freight_info()","for halt %i check connection",pos);
			// only add it, if there is not another thing waiting with the same via but another destination
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && 
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					wlist[i].get_zwischenziel() == wlist[pos].get_zwischenziel() &&
					( wlist[i].get_ziel() == wlist[i].get_zwischenziel() ) == ( wlist[pos].get_ziel() == wlist[pos].get_zwischenziel() )  )
				{
					wlist[i].menge += wlist[pos--].menge;
				}
			}
		}
		if(sort_mode == by_origin_amount && pos > 0) 
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() &&
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					wlist[i].get_origin() == wlist[pos].get_origin())
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if(sort_mode == by_origin && pos > 0) 
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() &&
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					wlist[i].get_origin() == wlist[pos].get_origin() &&
					wlist[i].get_index() == wlist[pos].get_index() &&
					wlist[i].get_ziel() == wlist[pos].get_ziel()) 
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if((sort_mode == by_name || sort_mode == by_via || sort_mode == by_amount) && pos > 0)
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && 
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					wlist[i].get_ziel() == wlist[pos].get_ziel())
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if((sort_mode == by_destination_detail || sort_mode == by_accommodation_detail) && pos > 0)
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() &&
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					wlist[i].get_zielpos() == wlist[pos].get_zielpos())
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if (sort_mode == by_wealth_detail && pos > 0)
		{
			for (int i = 0; i < pos; i++)
			{
				if (wlist[i].get_index() == wlist[pos].get_index() &&
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					wlist[i].get_class() == wlist[pos].get_class() &&
					wlist[i].get_zielpos() == wlist[pos].get_zielpos())
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}
		if (sort_mode == by_wealth_via && pos > 0)
		{
			for (int i = 0; i < pos; i++)
			{
				if (wlist[i].get_index() == wlist[pos].get_index() &&
					wlist[i].get_zwischenziel() == wlist[pos].get_zwischenziel() &&
					//(accommodation == all_classes || wlist[i].get_class() == accommodation) &&
					(wlist[i].get_ziel() == wlist[i].get_zwischenziel()) == (wlist[pos].get_ziel() == wlist[pos].get_zwischenziel()) &&
					wlist[i].get_class() == wlist[pos].get_class())
				{
					wlist[i].menge += wlist[pos--].menge;
				}
			}
		}

		/*	if (sort_mode == by_transfer_time && pos > 0)
		{
			for (int i = 0; i < pos; i++)
			{
				uint32 rt_i = wlist[i] == tc.ware ? tc.ready_time : NULL;
				uint32 rt_pos = wlist[pos] == tc.ware ? tc.ready_time : NULL;
				if (wlist[i].get_index() == wlist[pos].get_index() && (accommodation == all_classes || wlist[i].get_class() == accommodation) && rt_i == rt_pos)
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}*/

		pos++;
	}

	// if there, give the capacity for each freight
	slist_tpl<ware_t>                 const  dummy;
	slist_tpl<ware_t>                 const& list     = full_list ? *full_list : dummy;
	slist_tpl<ware_t>::const_iterator        full_i   = list.begin();
	slist_tpl<ware_t>::const_iterator const  full_end = list.end();

	// at least some capacity added?
	if(  pos != 0  ) {
		// sort the ware's list
		std::sort( wlist, wlist + pos, compare_ware );

		// print the ware's list to buffer - it should be in sort order by now!
		int last_goods_index = -1;
		int last_ware_catg = -1;
		int last_ware_class = -1;
		bool new_section = false;
		uint32 total_pass_mail = 0;
		uint32 last_capacity = 0;
		ware_t last;
		const bool sorting_by_wealth = sortby == by_wealth_detail || sortby == by_wealth_via ? true : false;
		const bool sorting_by_accommodation = sortby == by_accommodation_detail || sortby == by_accommodation_via ? true : false;	

		for (int j = 0; j < pos; j++) 
		{
			halthandle_t const halt = wlist[j].get_ziel();
			halthandle_t const via_halt = wlist[j].get_zwischenziel();
			halthandle_t const origin_halt = wlist[j].get_origin();

			const char * name = translator::translate("unknown");
			if (halt.is_bound()) {
				name = halt->get_name();
			}

			ware_t const& ware = wlist[j];
			//if(ware.get_class() != accommodation && accommodation != all_classes)
			//{
			//	continue;
			//}

			// Do we need to show a new category heading?
			if (last_goods_index != ware.get_index() && last_ware_catg != ware.get_catg()) {
				new_section = true;
				last_goods_index = ware.get_index();
				last_ware_catg = (ware.get_catg() != 0) ? ware.get_catg() : -1;
				// First, if we are dealing with passengers and mail, we might need to know how many i total we have on board
				total_pass_mail = 0;
				if (sorting_by_wealth && (ware.get_index() == goods_manager_t::INDEX_PAS || ware.get_index() == goods_manager_t::INDEX_MAIL))
				{
					for (int i = j; i < pos; i++) {
						ware_t const& sumware = wlist[i];
						if (last_goods_index != sumware.get_index()) {
							if (last_ware_catg != sumware.get_catg()) {
								break;	// next category reached ...
							}
						}
						total_pass_mail += sumware.menge;
					}
				}
			}
			if (sorting_by_wealth && last_ware_class != ware.get_class())
			{
				new_section = true;
				last_ware_class = ware.get_class();
			}

			if (new_section)
			{
				sint32 sum = 0;

				for (int i = j; i < pos; i++)
				{
					ware_t const& sumware = wlist[i];
					if (last_goods_index != sumware.get_index())
					{
						if (last_ware_catg != sumware.get_catg())
						{
							break;	// next category reached ...
						}
					}
					if (!sorting_by_wealth || sumware.get_class() == last_ware_class)
					{
						sum += sumware.menge;
					}
				}

				
				// special freight => handle differently
				last_ware_catg = (ware.get_catg() != 0) ? ware.get_catg() : -1;

				// display all ware
				if (full_list == NULL || full_list->get_count() == 0)
				{
					add_ware_heading(buf, sum, accommodation_capacity, &ware, what_doing, sorting_by_accommodation ? accommodation : last_ware_class, total_pass_mail, show_empty);
					total_pass_mail = 0;
				}
				else
				{
					// ok, we have a list of freight
					bool heading_added = false;
					while (full_i != full_end) 
					{
						ware_t const& current = *full_i++;
						last = current;
						if (last_goods_index == current.get_index() || last_ware_catg == current.get_catg() || last_ware_class == current.get_class()) 
						{
							add_ware_heading(buf, sum, current.menge, &current, what_doing, sorting_by_accommodation ? accommodation : last_ware_class, total_pass_mail, show_empty);
							last_capacity = current.menge;
							heading_added = true;
							total_pass_mail = 0;
							break;
						}
						else
						{
							add_ware_heading(buf, 0, current.menge, &current, what_doing, sorting_by_accommodation ? accommodation : last_ware_class, total_pass_mail, show_empty);
							last_capacity = current.menge;
							heading_added = true;
							total_pass_mail = 0;
						}
					}

					if (new_section && !heading_added)
					{
						add_ware_heading(buf, sum, last_capacity, &last, what_doing, sorting_by_accommodation ? accommodation : last_ware_class, total_pass_mail, show_empty);
						total_pass_mail = 0;
					}
				}

				new_section = false;
			}

			// Classes preparations.
			// Only show the wealth if we are not already sorting by wealth
			bool show_classes = false;
			char g_class_untranslated[32] = "\0";
			char g_class_text[32] = "\0";
			char g_class_alone[32] = "\0";
			if (!sorting_by_wealth)
			{
				if (ware.is_passenger())
				{
					sprintf(g_class_untranslated, "p_class[%u]", ware.get_class());
				}
				if (ware.is_mail())
				{
					sprintf(g_class_untranslated, "m_class[%u]", ware.get_class());
				}
				sprintf(g_class_text, "; %s", translator::translate(g_class_untranslated));
				sprintf(g_class_alone, " (%s)", translator::translate(g_class_untranslated));

			}
			// detail amount
			goods_desc_t const& desc = *ware.get_desc();
			buf.printf("   %u%s %s %c ", ware.menge, translator::translate(desc.get_mass()), translator::translate(desc.get_name()), ">>>>><>>>>>"[sortby]); // one ">" per sort mode..

			
		/*	const sint64 current_time = welt->get_ticks();
			char transfer_time_left[32] = "";
			if (halt->get_transferring_cargoes_count() > 0)
			{
				transferring_cargo_t tc;

			//	uint32 rt_i = wlist[j] == tc.ware ? tc.ready_time : NULL;
				const uint32 tc_ready_time = tc.ware == ware ? world()->ticks_to_seconds(tc.ready_time - current_time) : NULL;
			//	const uint32 ready_seconds = world()->ticks_to_seconds(tc.ready_time - current_time);				
				welt->sprintf_ticks(transfer_time_left, sizeof(transfer_time_left), tc_ready_time);
			}
		*/


			// the target name is not correct for the via sort
			if(sortby != by_via_sum && sortby != by_origin_amount && sortby != by_wealth_via && sortby != by_accommodation_via)
			{
				koord zielpos = ware.get_zielpos();
				const grund_t* gr = welt->lookup_kartenboden(zielpos);
				const gebaeude_t* const gb = gr ? gr->get_building() : NULL;
				cbuffer_t dbuf;
				if (gb)
				{
					gb->get_description(dbuf);
				}
				else
				{
					dbuf.append(translator::translate("Unknown destination"));
				}
				const stadt_t* city = welt->get_city(zielpos);

				if(ware.is_passenger() && (sortby == by_destination_detail || sortby == by_wealth_detail || sortby == by_accommodation_detail))
				{
					const char* trip_type = (ware.is_commuting_trip ? translator::translate("commuting") : translator::translate("visiting"));

					if(city)
					{
						buf.printf("%s <%i, %i> (%s; %s%s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, city->get_name(), trip_type, g_class_text);
					}
					else 
					{
						buf.printf("%s <%i, %i> (%s%s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, trip_type, g_class_text);
					}
				}
				else if (ware.is_mail() && (sortby == by_destination_detail || sortby == by_wealth_detail || sortby == by_accommodation_detail))
				{
					if (city)
					{
						buf.printf("%s <%i, %i> (%s%s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, city->get_name(), g_class_text);
					}
					else
					{
						buf.printf("%s <%i, %i>%s\n     ", dbuf.get_str(), zielpos.x, zielpos.y, g_class_alone);
					}
				}
				else if(sortby == by_destination_detail)
				{
					if(city)
					{
						buf.printf("%s <%i, %i> (%s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, city->get_name());
					}
					else
					{
						buf.printf("%s <%i, %i>\n     ", dbuf.get_str(), zielpos.x, zielpos.y);
					}
				}
			}

			if(sortby == by_name || sortby == by_destination_detail || sortby == by_amount || sortby == by_origin || (sortby == by_via_sum && via_halt == halt) || sortby == by_via || sortby == by_wealth_detail || (sortby == by_wealth_via && via_halt == halt) || sortby == by_accommodation_detail || (sortby == by_accommodation_via && via_halt == halt))
			{
				const char *destination_name = translator::translate("unknown");
				if(halt.is_bound()) 
				{
					destination_name = halt->get_name();
				}
				if(sortby == by_destination_detail || sortby==by_wealth_detail || sortby == by_accommodation_detail)
				{
					buf.printf(translator::translate(" via %s"), destination_name);
				}
				else
				{
					buf.printf(destination_name);
				}
			}

			if(sortby == by_origin_amount)
			{
				const char *origin_name = translator::translate("unknown");
				if(origin_halt.is_bound()) 
				{
					origin_name = origin_halt->get_name();
				}
				buf.printf(origin_name);
			}
			
			if(via_halt != halt && (sortby == by_via || sortby == by_via_sum || sortby == by_wealth_via || sortby == by_accommodation_via))
			{
				const char *via_name = translator::translate("unknown");
				if(via_halt.is_bound()) 
				{
					via_name = via_halt->get_name();
				}
				buf.printf(translator::translate(" via %s"), via_name);
			}
			if(sortby == by_origin)
			{
				const char *origin_name = translator::translate("unknown");
				if(origin_halt.is_bound()) 
				{
					origin_name = origin_halt->get_name();
				}
				if (ware.is_passenger())
				{
					const char* trip_type = (ware.is_commuting_trip ? translator::translate("commuting") : translator::translate("visiting"));

					buf.printf(translator::translate(" from %s (%s%s)"), origin_name, trip_type, g_class_text); // With class entries
				}
				else if (ware.is_mail())
				{
					buf.printf(translator::translate(" from %s%s"), origin_name, g_class_alone); // With class entries
				}
			}

			buf.append("\n");
			// debug end
		}
	}	
	// Is there any empty accommodation in the convoy?
	else if (accommodation_capacity > 0)
	{
		add_ware_heading(buf, 0, accommodation_capacity, accommodation_ware, what_doing, accommodation, 0, show_empty);
	}

	// still entires left?
	for(  ; full_i != full_end; ++full_i  ) {
		ware_t const& g = *full_i;
		add_ware_heading(buf, 0, g.menge, &g, what_doing, g.get_class(), 0, show_empty);
	}



#ifdef _MSC_VER
	if(warray_size >= max_stack_size)
#endif
	{
		free(wlist);
	}
}
