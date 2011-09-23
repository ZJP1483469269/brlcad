/*                         S I M U L A T E . C
 * BRL-CAD
 *
 * Copyright (c) 2008-2011 United States Government as represented by
 * the U.S. Army Research Laboratory.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this file; see the file named COPYING for more
 * information.
 */
/** @file libged/simulate/simulate.c
 *
 * The simulate command.
 *
 * Routines related to performing physics on passed objects only
 * TODO : Adds flags to control AABB and object state display
 * TODO : Finish integrating rt into collision detection
 * 
 */

#include "common.h"

#ifdef HAVE_BULLET

#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "bio.h"

#include "vmath.h"
#include "db.h"
#include "bu.h"
#include "raytrace.h"
#include "../ged_private.h"
#include "simulate.h"


/* The C++ simulation function */
extern int run_simulation(struct simulation_params *sim_params);

/* Physics attributes that are currently checked for in regions */
#define MAX_SIM_ATTRIBS 6

/* DO NOT CHANGE THE ORDER of attribs in sim_attrib as the code assumes the current order
 * add new sim attribs at the bottom and increase MAX_SIM_ATTRIBS
 */
static const char* sim_attrib[MAX_SIM_ATTRIBS] = {
    "simulate:mass",
    "simulate:force",
    "simulate:linear_velocity",
    "simulate:angular_velocity",
    "simulate:restitution",
    "simulate:friction"
};

/* Maximum allowed values of scalars and components of vectors
 * These are to check for correctness of user set values, not imposed by physics
 */
static const fastf_t MAX_MASS = 500000;
static const fastf_t MAX_FORCE_COMP = 100000;
static const fastf_t MAX_LINEAR_VELOCITY_COMP = 100000;
static const fastf_t MAX_ANGULAR_VELOCITY_COMP = 100000;

/**
 * How to use simulate.Blissfully simple interface, more options will be added soon
 */
static void
print_usage(struct bu_vls *str)
{
    bu_vls_printf(str, "Usage: simulate <steps>\n\n");
    bu_vls_printf(str, "Currently this command adds all regions in the model database to a \n\
    	simulation having only gravity as a force. The objects should fall towards the ground plane XY.\n");
    bu_vls_printf(str, "The positions of the regions are set after <steps> number of simulation steps.\n");
    bu_vls_printf(str, "-f <n> <x> <y> <z>\t- Specifies frequency of update(eg 1/60 Hz)(WIP)\n");
    bu_vls_printf(str, "-t <x> <y> <z>\t\t  - Specifies time for which to run(alternative to -n)(WIP)\n");
    return;
}


/**
 * Prints a 16 by 16 transform matrix for debugging
 *
 */
void print_matrix(struct simulation_params *sim_params, char *rb_namep, mat_t t)
{
	int i, j;

	bu_vls_printf(sim_params->result_str, "------------Transformation matrix(%s)--------------\n",
			rb_namep);

	for (i=0 ; i<4 ; i++) {
		for (j=0 ; j<4 ; j++) {
			bu_vls_printf(sim_params->result_str, "t[%d]: %f\t", (i*4 + j), t[i*4 + j] );
		}
		bu_vls_printf(sim_params->result_str, "\n");
	}

	bu_vls_printf(sim_params->result_str, "-------------------------------------------------------\n");
}


/**
 * Prints a struct rigid_body for debugging, more members will be printed later
 */
void print_rigid_body(struct rigid_body *rb)
{
    bu_log("Rigid Body : \"%s\", state = %d\n", rb->rb_namep, rb->state);
}


/**
 * Deletes a prim/comb if it exists
 * TODO : lower to librt
 */
int kill(struct ged *gedp, char *name)
{
    char *cmd_args[5];

    /* Check if the duplicate already exists, and kill it if so */
    if (db_lookup(gedp->ged_wdbp->dbip, name, LOOKUP_QUIET) != RT_DIR_NULL) {
        bu_log("kill: WARNING \"%s\" exists, deleting it\n", name);
        cmd_args[0] = "kill";
        cmd_args[1] = name;
        cmd_args[2] = (char *)0;

        if(ged_kill(gedp, 2, (const char **)cmd_args) != GED_OK){
            bu_log("kill: ERROR Could not delete existing \"%s\"\n", name);
            return GED_ERROR;
        }
    }

    return GED_OK;
}


/**
 * Deletes and duplicates the prim/comb passed in dp as new_name
 * TODO : lower to librt
 */
int kill_copy(struct ged *gedp, struct directory *dp, char* new_name)
{
    char *cmd_args[5];
    int rv;

    if( kill(gedp, new_name) != GED_OK){
		bu_log("kill_copy: ERROR Could not delete existing \"%s\"\n", new_name);
		return GED_ERROR;
	}

    /* Copy the passed prim/comb */
    cmd_args[0] = "copy";
    cmd_args[1] = dp->d_namep;
    cmd_args[2] = new_name;
    cmd_args[3] = (char *)0;
    rv = ged_copy(gedp, 3, (const char **)cmd_args);
    if (rv != GED_OK){
        bu_log("kill_copy: ERROR Could not copy \"%s\" to \"%s\"\n", dp->d_namep,
                new_name);
        return GED_ERROR;
    }

    return GED_OK;
}


/**
 * Adds a prim/comb to an existing comb or creates it if not existing
 * TODO : lower to librt
 */
int add_to_comb(struct ged *gedp, char *target, char *add)
{
	char *cmd_args[5];
	int rv;

	cmd_args[0] = "comb";
	cmd_args[1] = target;
	cmd_args[2] = "u";
	cmd_args[3] = add;
	cmd_args[4] = (char *)0;
	rv = ged_comb(gedp, 4, (const char **)cmd_args);
	if (rv != GED_OK){
		bu_log("add_to_comb: ERROR Could not add \"%s\" to the combination \"%s\"\n",
				target, add);
		return GED_ERROR;
	}

	return GED_OK;
}


/**
 * Parses a string containing 3 floating point components into a vector vect_t
 *
 */
int parse_vector(vect_t vec, const char *str)
{
	char *argv[ELEMENTS_PER_VECT];
	char *end;
	VSETALL(vec, 0);

	if(strlen(str)){
		bu_argv_from_string((char **)argv, ELEMENTS_PER_VECT, (char *)str);

		vec[0] = strtod(argv[0], &end);
		vec[1] = strtod(argv[1], &end);
		vec[2] = strtod(argv[2], &end);
	}

	/*bu_log("parse_vector: str = \"%s\" , vec = (%f, %f, %f)\n",	str, vec[0], vec[1], vec[2]);*/


	return GED_OK;
}


/**
 * Adds physics attributes to a node of the rigid_body list to be dispatched to physics
 *
 */
int add_physics_attributes(struct ged *gedp, struct rigid_body *current_node, struct directory *ndp)
{
	unsigned int i;
	struct bu_attribute_value_set avs;
	struct bu_attribute_value_pair *avpp;
	const char *val, *val_cpy;
	fastf_t simulate_mass;
	vect_t  simulate_force, simulate_linear_velocity, simulate_angular_velocity;

	bu_avs_init_empty(&avs);
	if (db5_get_attributes(gedp->ged_wdbp->dbip, &avs, ndp)) {
		bu_vls_printf(gedp->ged_result_str, "add_regions : ERROR : Cannot get attributes for object %s\n",
												ndp->d_namep);
		return GED_ERROR;
	}

	/* Set sim attributes to default values */
	current_node->mass = 0;
	VSETALL(current_node->force, 0);
	VSETALL(current_node->force_position, 0);
	VSETALL(current_node->linear_velocity, 0);
	VSETALL(current_node->angular_velocity, 0);

	/* Sort attribute-value set array by attribute name */
	qsort(&avs.avp[0], avs.count, sizeof(struct bu_attribute_value_pair), _ged_cmpattr);

	for (i = 0; i < MAX_SIM_ATTRIBS; i++) {
		val = bu_avs_get(&avs, sim_attrib[i]);
		if (val) {
			bu_log("add_regions : Object %s has %s = %s\n", ndp->d_namep, sim_attrib[i], val);
			val_cpy = bu_strdup(val);

			switch(i){
			/* Mass */
			case 0:
				simulate_mass = atoi(val_cpy);
				if(simulate_mass < 0 || simulate_mass > MAX_MASS){
					bu_log("add_regions : WARNING %s should be between %f and %f, set to %f\n",
							sim_attrib[i], 0.0, MAX_MASS, 0.0);
					current_node->mass = 0;
				}
				else
					current_node->mass = simulate_mass;
				break;
			/* Custom Force */
			case 1:
				parse_vector(simulate_force, val_cpy);
				if(abs(simulate_force[0]) > MAX_FORCE_COMP ||
				   abs(simulate_force[1]) > MAX_FORCE_COMP ||
				   abs(simulate_force[2]) > MAX_FORCE_COMP){
					bu_log("add_regions : WARNING %s should be between %f and %f, set to %f\n",
							sim_attrib[i], 0.0, MAX_FORCE_COMP, 0.0);
					VSETALL(current_node->force, 0);
				}
				else
					VMOVE(current_node->force, simulate_force);
				break;
			/* Linear Velocity */
			case 2:
				parse_vector(simulate_linear_velocity, val_cpy);
				if(abs(simulate_linear_velocity[0]) > MAX_LINEAR_VELOCITY_COMP ||
				   abs(simulate_linear_velocity[1]) > MAX_LINEAR_VELOCITY_COMP ||
				   abs(simulate_linear_velocity[2]) > MAX_LINEAR_VELOCITY_COMP){
					bu_log("add_regions : WARNING %s should be between %f and %f, set to %f\n",
							sim_attrib[i], 0.0, MAX_LINEAR_VELOCITY_COMP, 0.0);
					VSETALL(current_node->linear_velocity, 0);
				}
				else
					VMOVE(current_node->linear_velocity, simulate_linear_velocity);
				break;
			/* Angular Velocity */
			case 3:
				parse_vector(simulate_angular_velocity, val_cpy);
				if(abs(simulate_angular_velocity[0]) > MAX_ANGULAR_VELOCITY_COMP ||
				   abs(simulate_angular_velocity[1]) > MAX_ANGULAR_VELOCITY_COMP ||
				   abs(simulate_angular_velocity[2]) > MAX_ANGULAR_VELOCITY_COMP){
					bu_log("add_regions : WARNING %s should be between %f and %f, set to %f\n",
							sim_attrib[i], 0.0, MAX_ANGULAR_VELOCITY_COMP, 0.0);
					VSETALL(current_node->angular_velocity, 0);
				}
				else
					VMOVE(current_node->angular_velocity, simulate_angular_velocity);
				break;
			default:
				bu_log("add_regions : WARNING : simulation attribute %s not recognized!\n",
						sim_attrib[i]);
			} /* switch */
		} /* if */
	} /* for */

	/* Just list all the attributes */
	for (i = 0, avpp = avs.avp; i < avs.count; i++, avpp++) {
		bu_log("Attribute : %s {%s} ", avpp->name, avpp->value);
	}

	bu_avs_free(&avs);

	return GED_OK;

}

/**
 * Add the list of regions in the model to the rigid bodies list in
 * simulation parameters. This function will duplicate the existing regions
 * prefixing "sim_" to the new region and putting them all under a new
 * comb "sim.c". It will skip over any existing regions with "sim_" in the name
 */
int add_regions(struct ged *gedp, struct simulation_params *sim_params)
{
    struct directory *dp, *ndp;
    char *prefixed_name;
    char *prefix = "sim_";
    size_t  prefix_len, prefixed_name_len;
    point_t rpp_min, rpp_max;
    unsigned int i;
    struct rigid_body *prev_node = NULL, *current_node;


    /* Kill the existing sim comb */
    kill(gedp, sim_params->sim_comb_name);
    sim_params->num_bodies = 0;

    /* Walk the directory list duplicating all regions only, skip some regions */
    for (i = 0; i < RT_DBNHASH; i++)
        for (dp = gedp->ged_wdbp->dbip->dbi_Head[i]; dp != RT_DIR_NULL; dp = dp->d_forw) {
            if ( (dp->d_flags & RT_DIR_HIDDEN) ||  /* check for hidden comb/prim */
                !(dp->d_flags & RT_DIR_REGION)     /* check if region */
            )
                continue;

            if (strstr(dp->d_namep, prefix)){
            	bu_log("add_regions: Skipping \"%s\" due to \"%s\" in name\n",
                        dp->d_namep, prefix);
                continue;
            }

            /* Duplicate the region */
            prefix_len = strlen(prefix);
            prefixed_name_len = strlen(prefix)+strlen(dp->d_namep)+1;
            prefixed_name = (char *)bu_malloc(prefixed_name_len, "Adding sim_ prefix");
            bu_strlcpy(prefixed_name, prefix, prefix_len + 1);
            bu_strlcat(prefixed_name + prefix_len, dp->d_namep, prefixed_name_len - prefix_len);

            kill_copy(gedp, dp, prefixed_name);
            bu_log("add_regions: Copied \"%s\" to \"%s\"\n", dp->d_namep, prefixed_name);

            /* Get the directory pointer for the duplicate object just added */
            if ((ndp=db_lookup(gedp->ged_wdbp->dbip, prefixed_name, LOOKUP_QUIET)) == RT_DIR_NULL) {
            	bu_log("add_regions: db_lookup(%s) failed", prefixed_name);
                return GED_ERROR;
            }

            /* Get its BB */
            if(rt_bound_internal(gedp->ged_wdbp->dbip, ndp, rpp_min, rpp_max) == 0)
            	bu_log("add_regions: Got the BB for \"%s\" as min {%f %f %f} max {%f %f %f}\n", ndp->d_namep,
                        rpp_min[0], rpp_min[1], rpp_min[2],
                        rpp_max[0], rpp_max[1], rpp_max[2]);
            else{
            	bu_log("add_regions: ERROR Could not get the BB\n");
                return GED_ERROR;
            }

            /* Add to simulation list */
            current_node = (struct rigid_body *)bu_malloc(sizeof(struct rigid_body), "rigid_body: current_node");
            current_node->index = sim_params->num_bodies;
            current_node->rb_namep = bu_strdup(prefixed_name);
            current_node->dp = ndp;
            VMOVE(current_node->bb_min, rpp_min);
            VMOVE(current_node->bb_max, rpp_max);
            current_node->next = NULL;

            /* Get BB length, width, height */
            current_node->bb_dims[0] = current_node->bb_max[0] - current_node->bb_min[0];
            current_node->bb_dims[1] = current_node->bb_max[1] - current_node->bb_min[1];
            current_node->bb_dims[2] = current_node->bb_max[2] - current_node->bb_min[2];

            bu_log("add_regions: Dimensions of this BB : %f %f %f\n",
                    current_node->bb_dims[0], current_node->bb_dims[1], current_node->bb_dims[2]);

            /* Get BB position in 3D space */
            current_node->bb_center[0] = current_node->bb_min[0] + current_node->bb_dims[0]/2;
            current_node->bb_center[1] = current_node->bb_min[1] + current_node->bb_dims[1]/2;
            current_node->bb_center[2] = current_node->bb_min[2] + current_node->bb_dims[2]/2;

            /* Get its physics attributes */
            add_physics_attributes(gedp, current_node, ndp);

            /* Setup the linked list */
            if(prev_node == NULL){ /* first node */
                prev_node = current_node;
                sim_params->head_node = current_node;
            }
            else{                   /* past 1st node now */
                prev_node->next = current_node;
                prev_node = prev_node->next;
            }

            /* Add the new region to the simulation result */
            add_to_comb(gedp, sim_params->sim_comb_name, prefixed_name);

            sim_params->num_bodies++;
        }




    /* Show list of objects to be added to the sim : keep for debugging as of now */
    /*    bu_log("add_regions: The following %d regions will participate in the sim : \n", sim_params->num_bodies);
    for (current_node = sim_params->head_node; current_node != NULL; current_node = current_node->next) {
        print_rigid_body(current_node);
    }*/

    return GED_OK;

}


/**
 * This function draws the bounding box around a comb as reported by
 * Bullet
 * TODO : this should be used with a debugging flag
 * TODO : this function will soon be lowered to librt
 *
 */
int insertAABB(struct ged *gedp, struct simulation_params *sim_params,
								 struct rigid_body *current_node)
{
	char* cmd_args[28];
	char buffer[20];
	int rv;
	char *prefixed_name, *prefixed_reg_name;
	char *prefix = "bb_";
	char *prefix_reg = "bb_reg_";
	size_t  prefix_len, prefixed_name_len;
	point_t v;

	/* Prepare prefixed bounding box primitive name */
	prefix_len = strlen(prefix);
	prefixed_name_len = strlen(prefix)+strlen(current_node->rb_namep)+1;
	prefixed_name = (char *)bu_malloc(prefixed_name_len, "Adding bb_ prefix");
	bu_strlcpy(prefixed_name, prefix, prefix_len + 1);
	bu_strlcat(prefixed_name + prefix_len, current_node->rb_namep,
			   prefixed_name_len - prefix_len);

	/* Prepare prefixed bounding box region name */
	prefix_len = strlen(prefix_reg);
	prefixed_name_len = strlen(prefix_reg) + strlen(current_node->rb_namep) + 1;
	prefixed_reg_name = (char *)bu_malloc(prefixed_name_len, "Adding bb_reg_ prefix");
	bu_strlcpy(prefixed_reg_name, prefix_reg, prefix_len + 1);
	bu_strlcat(prefixed_reg_name + prefix_len, current_node->rb_namep,
			   prefixed_name_len - prefix_len);

	/* Delete existing bb prim and region */
	rv = kill(gedp, prefixed_name);
	if (rv != GED_OK){
		bu_log("insertAABB: ERROR Could not delete existing bounding box arb8 : %s \
				so NOT attempting to add new bounding box\n", prefixed_name);
		return GED_ERROR;
	}

	rv = kill(gedp, prefixed_reg_name);
	if (rv != GED_OK){
		bu_log("insertAABB: ERROR Could not delete existing bounding box region : %s \
				so NOT attempting to add new region\n", prefixed_reg_name);
		return GED_ERROR;
	}

	/* Setup the simulation result group union-ing the new objects */
	cmd_args[0] = "in";
	cmd_args[1] = bu_strdup(prefixed_name);
	cmd_args[2] = "arb8";

	/* Front face vertices */
	/* v1 */
	v[0] = current_node->btbb_center[0] + current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] + current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] - current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[3] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[4] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[5] = bu_strdup(buffer);

	/* v2 */
	v[0] = current_node->btbb_center[0] + current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] + current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] + current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[6] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[7] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[8] = bu_strdup(buffer);

	/* v3 */
	v[0] = current_node->btbb_center[0] + current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] - current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] + current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[9]  = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[10] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[11] = bu_strdup(buffer);

	/* v4 */
	v[0] = current_node->btbb_center[0] + current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] - current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] - current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[12] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[13] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[14] = bu_strdup(buffer);

	/* Back face vertices */
	/* v5 */
	v[0] = current_node->btbb_center[0] - current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] + current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] - current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[15] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[16] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[17] = bu_strdup(buffer);

	/* v6 */
	v[0] = current_node->btbb_center[0] - current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] + current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] + current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[18] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[19] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[20] = bu_strdup(buffer);

	/* v7 */
	v[0] = current_node->btbb_center[0] - current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] - current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] + current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[21] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[22] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[23] = bu_strdup(buffer);

	/* v8 */
	v[0] = current_node->btbb_center[0] - current_node->btbb_dims[0]/2;
	v[1] = current_node->btbb_center[1] - current_node->btbb_dims[1]/2;
	v[2] = current_node->btbb_center[2] - current_node->btbb_dims[2]/2;
	sprintf(buffer, "%f", v[0]); cmd_args[24] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[1]); cmd_args[25] = bu_strdup(buffer);
	sprintf(buffer, "%f", v[2]); cmd_args[26] = bu_strdup(buffer);

	/* Finally make the bb primitive, phew ! */
	cmd_args[27] = (char *)0;
	rv = ged_in(gedp, 27, (const char **)cmd_args);
	if (rv != GED_OK){
		bu_log("insertAABB: WARNING Could not draw bounding box for \"%s\"\n",
				current_node->rb_namep);
	}

	/* Make the region for the bb primitive */
	add_to_comb(gedp, prefixed_reg_name, prefixed_name);

	/* Adjust the material for region to be almost transparent */
	cmd_args[0] = "mater";
	cmd_args[1] = bu_strdup(prefixed_reg_name);
	cmd_args[2] = "plastic tr 0.9";
	cmd_args[3] = "210";
	cmd_args[4] = "0";
	cmd_args[5] = "100";
	cmd_args[6] = "0";
	cmd_args[7] = (char *)0;
	rv = ged_mater(gedp, 7, (const char **)cmd_args);
	if (rv != GED_OK){
		bu_log("insertAABB: WARNING Could not adjust the material for \"%s\"\n",
				prefixed_reg_name);
	}

	/* Add the region to the result of the sim so it will be drawn too */
	add_to_comb(gedp, sim_params->sim_comb_name, prefixed_reg_name);

	return GED_OK;

}


/**
 * This function colors the passed comb. It's for showing the current
 * state of the object inside the physics engine.
 * TODO : this should be used with a debugging flag
 *
 */
int apply_color(struct ged *gedp, char* rb_namep, unsigned char r,
											      unsigned char g,
											      unsigned char b )
{
	struct directory *dp = NULL;
	struct rt_comb_internal *comb = NULL;
	struct rt_db_internal intern;
	struct bu_attribute_value_set avs;

	/* Look up directory pointer for the passed comb name */
	GED_DB_LOOKUP(gedp, dp, rb_namep, LOOKUP_NOISY, GED_ERROR);
	GED_CHECK_COMB(gedp, dp, GED_ERROR);
	GED_DB_GET_INTERNAL(gedp, &intern, dp, (fastf_t *)NULL, &rt_uniresource, GED_ERROR);

	/* Get a comb from the internal format */
	comb = (struct rt_comb_internal *)intern.idb_ptr;
	RT_CK_COMB(comb);

	/* Set the color related members */
	comb->rgb[0] = r;
	comb->rgb[1] = g;
	comb->rgb[2] = b;
	comb->rgb_valid = 1;
	comb->inherit = 0;

	/* Get the current attribute set of the comb from the db */
	bu_avs_init_empty(&avs);
	if (db5_get_attributes(gedp->ged_wdbp->dbip, &avs, dp)) {
		bu_vls_printf(gedp->ged_result_str, "apply_transforms: ERROR Cannot get attributes for object %s\n", dp->d_namep);
		bu_avs_free(&avs);
		return GED_ERROR;
	}

	/* Sync the changed attributes with the old ones */
	db5_standardize_avs(&avs);
	db5_sync_comb_to_attr(&avs, comb);
	db5_standardize_avs(&avs);

	/* Put back in db to allow drawing */
	GED_DB_PUT_INTERNAL(gedp, dp, &intern, &rt_uniresource, GED_ERROR);
	if (db5_update_attributes(dp, &avs, gedp->ged_wdbp->dbip)) {
		bu_vls_printf(gedp->ged_result_str, "apply_transforms: ERROR failed to update attributes\n");
		bu_avs_free(&avs);
		return GED_ERROR;
	}

	bu_avs_free(&avs);
	return GED_OK;
}


/**
 * The libged physics simulation function :
 * Check flags, adds regions to simulation parameters, runs the simulation
 * applies the transforms, frees memory
 */
int
ged_simulate(struct ged *gedp, int argc, const char *argv[])
{
    int rv;
    struct simulation_params sim_params;
    static const char *sim_comb_name = "sim.c";
    static const char *ground_plane_name = "sim_gp.r";
    struct rigid_body *current_node, *next_node;

    GED_CHECK_DATABASE_OPEN(gedp, GED_ERROR);
    GED_CHECK_ARGC_GT_0(gedp, argc, GED_ERROR);

    /* Initialize result */
    bu_vls_trunc(gedp->ged_result_str, 0);

    /* Must be wanting help */
    if (argc == 1) {
        print_usage(gedp->ged_result_str);
        return GED_HELP;
    }

    if (argc < 2) {
        bu_vls_printf(gedp->ged_result_str, "Usage: %s <steps>", argv[0]);
        return GED_ERROR;
    }

    /* Make a list containing the bb and existing transforms of all the objects in the model
     * which will participate in the simulation
     */
    sim_params.duration = atoi(argv[1]);
    sim_params.dbip = gedp->ged_wdbp->dbip;
    sim_params.result_str = gedp->ged_result_str;
    sim_params.sim_comb_name = bu_strdup(sim_comb_name);
    sim_params.ground_plane_name = bu_strdup(ground_plane_name);
    rv = add_regions(gedp, &sim_params);
    if (rv != GED_OK){
        bu_vls_printf(gedp->ged_result_str, "%s: ERROR while adding objects\n", argv[0]);
        return GED_ERROR;
    }

    /* This call will run physics for 1 step and put the resultant vel/forces into sim_params */
	rv = run_simulation(&sim_params);
	if (rv != GED_OK){
		bu_vls_printf(gedp->ged_result_str, "%s: ERROR while running the simulation\n", argv[0]);
		return GED_ERROR;
	}

	/* Free memory in rigid_body list */
	for (current_node = sim_params.head_node; current_node != NULL; ) {
		next_node = current_node->next;
		bu_free(current_node, "simulate : current_node");
		current_node = next_node;
	}

    bu_vls_printf(gedp->ged_result_str, "%s: The simulation result is in group : %s\n", argv[0], sim_comb_name);
                                                     
    /* Draw the result : inserting it in argv[1] will cause it to be automatically drawn in the cmd_wrapper */
    argv[1] = sim_comb_name;
    argv[2] = (char *)0;

    return GED_OK;
}


#else

#include "../ged_private.h"

/**
 * Dummy simulation function in case no physics library found
 */
int
ged_simulate(struct ged *gedp, int argc, const char *argv[])
{
    GED_CHECK_DATABASE_OPEN(gedp, GED_ERROR);
    GED_CHECK_ARGC_GT_0(gedp, argc, GED_ERROR);

    /* Initialize result */
    bu_vls_trunc(gedp->ged_result_str, 0);

    bu_vls_printf(gedp->ged_result_str, "%s : ERROR This command is disabled due to the absence of a physics library",
            argv[0]);
    return GED_ERROR;
}

#endif






/*
 * Local Variables:
 * tab-width: 8
 * mode: C
 * indent-tabs-mode: t
 * c-file-style: "stroustrup"
 * End:
 * ex: shiftwidth=4 tabstop=8
 */
