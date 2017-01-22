#include <stdio.h>
#include "../../simdebug.h"
#include "../../bauer/warenbauer.h"

#include "good_reader.h"
#include "../obj_node_info.h"
#include "../ware_besch.h"
#include "../../network/pakset_info.h"


void good_reader_t::register_obj(obj_desc_t *&data)
{
	ware_desc_t *desc = static_cast<ware_desc_t *>(data);

	warenbauer_t::register_desc(desc);
	DBG_DEBUG("good_reader_t::register_obj()","loaded good '%s'", desc->get_name());

	obj_for_xref(get_type(), desc->get_name(), data);

	checksum_t *chk = new checksum_t();
	desc->calc_checksum(chk);
	pakset_info_t::append(desc->get_name(), chk);
}


bool good_reader_t::successfully_loaded() const
{
	return warenbauer_t::alles_geladen(); //"Alles geladen" = "Everything laoded" (Babelfish)
}


obj_desc_t * good_reader_t::read_node(FILE *fp, obj_node_info_t &node)
{
	ALLOCA(char, desc_buf, node.size);

	ware_desc_t *desc = new ware_desc_t();

	// some defaults
	desc->speed_bonus = 0;
	desc->weight_per_unit = 100;
	desc->color = 255;

	// Hajo: Read data
	fread(desc_buf, node.size, 1, fp);

	char * p = desc_buf;

	// Hajo: old versions of PAK files have no version stamp.
	// But we know, the higher most bit was always cleared.

	const uint16 v = decode_uint16(p);
	int version = v & 0x8000 ? v & 0x7FFF : 0;

	// Whether the read file is from Simutrans-Experimental
	//@author: jamespetts
	const bool experimental = version > 0 ? v & EXP_VER : false;
	uint16 experimental_version = 0;
	if(experimental)
	{
		// Experimental version to start at 0 and increment.
		version = version & EXP_VER ? version & 0x3FFF : 0;
		while(version > 0x100)
		{
			version -= 0x100;
			experimental_version ++;
		}
		experimental_version -= 1;
	}

	if(version == 1) {
		// Versioned node, version 1
		desc->base_values.append(fare_stage_t(0, decode_uint16(p)));
		desc->catg = (uint8)decode_uint16(p);
		desc->speed_bonus = decode_uint16(p);
		desc->weight_per_unit = 100;

	} else if(version == 2) {
		// Versioned node, version 2
		desc->base_values.append(fare_stage_t(0, decode_uint16(p)));
		desc->catg = (uint8)decode_uint16(p);
		desc->speed_bonus = decode_uint16(p);
		desc->weight_per_unit = decode_uint16(p);

	} else if(version == 3) {
		// Versioned node, version 3
		const uint16 base_value = decode_uint16(p);
		desc->catg = decode_uint8(p);
		desc->speed_bonus = decode_uint16(p);
		desc->weight_per_unit = decode_uint16(p);
		desc->color = decode_uint8(p);
		if(experimental)
		{
			const uint8 fare_stages = decode_uint8(p);
			if(fare_stages > 0)
			{
				// The base value is not used if fare stages are used. 
				for(int i = 0; i < fare_stages; i ++)
				{
					const uint16 to_distance = decode_uint16(p);
					const uint16 val = decode_uint16(p);
					desc->base_values.append(fare_stage_t((uint32)to_distance, val));
				}
			}
			else
			{
				desc->base_values.append(fare_stage_t(0, base_value));
			}
		}
		else
		{
			desc->base_values.append(fare_stage_t(0, base_value));
		}
	}
	else {
		// old node, version 0
		desc->base_values.append(fare_stage_t(0, v));
		desc->catg = (uint8)decode_uint16(p);
	}

	DBG_DEBUG("good_reader_t::read_node()","version=%d value=%d catg=%d bonus=%d",version, desc->base_values.get_count() > 0 ? desc->base_values[0].price : 0, desc->catg, desc->speed_bonus);


  return desc;
}
