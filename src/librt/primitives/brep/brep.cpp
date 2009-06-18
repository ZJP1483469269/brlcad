/*                     B R E P . C P P
 * BRL-CAD
 *
 * Copyright (c) 2007-2009 United States Government as represented by
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
/** @addtogroup g_  */
/** @{ */
/** @file brep.cpp
 *
 * Implementation of a generalized Boundary Representation (BREP)
 * primitive using the openNURBS library.
 *
 */

#include "common.h"

#include <vector>
#include <list>
#include <iostream>
#include <algorithm>
#include <set>
#include <utility>

#define BN_VMATH_PREFIX_INDICES 1
#define ROOT_TOL 1.E-7

#include "vmath.h"

#include "brep.h"
#include "vector.h"

#include "raytrace.h"
#include "rtgeom.h"


#ifdef __cplusplus
extern "C" {
#endif

int
rt_brep_prep(struct soltab *stp, struct rt_db_internal* ip, struct rt_i* rtip);
void
rt_brep_print(register const struct soltab *stp);
int
rt_brep_shot(struct soltab *stp, register struct xray *rp, struct application *ap, struct seg *seghead);
void
rt_brep_norm(register struct hit *hitp, struct soltab *stp, register struct xray *rp);
void
rt_brep_curve(register struct curvature *cvp, register struct hit *hitp, struct soltab *stp);
int
rt_brep_class();
void
rt_brep_uv(struct application *ap, struct soltab *stp, register struct hit *hitp, register struct uvcoord *uvp);
void
rt_brep_free(register struct soltab *stp);
int
rt_brep_plot(struct bu_list *vhead, struct rt_db_internal *ip, const struct rt_tess_tol *ttol, const struct bn_tol *tol);
int
rt_brep_tess(struct nmgregion **r, struct model *m, struct rt_db_internal *ip, const struct rt_tess_tol *ttol, const struct bn_tol *tol);
int
rt_brep_export5(struct bu_external *ep, const struct rt_db_internal *ip, double local2mm, const struct db_i *dbip);
int
rt_brep_import5(struct rt_db_internal *ip, const struct bu_external *ep, register const fastf_t *mat, const struct db_i *dbip);
void
rt_brep_ifree(struct rt_db_internal *ip, struct resource *resp);
int
rt_brep_describe(struct bu_vls *str, const struct rt_db_internal *ip, int verbose, double mm2local);
int
rt_brep_tclget(Tcl_Interp *interp, const struct rt_db_internal *intern, const char *attr);
int
rt_brep_tcladjust(Tcl_Interp *interp, struct rt_db_internal *intern, int argc, char **argv);
int
rt_brep_params(struct pc_pc_set *, const struct rt_db_internal *ip);
#ifdef __cplusplus
}
#endif


/* FIXME: fugly */
static int hit_count = 0;

//debugging
static int icount = 0;

/********************************************************************************
 * Auxiliary functions
 ********************************************************************************/


using namespace brlcad;

ON_Ray toXRay(struct xray* rp) {
    ON_3dPoint pt(rp->r_pt);
    ON_3dVector dir(rp->r_dir);
    return ON_Ray(pt, dir);
}

//--------------------------------------------------------------------------------
// specific
struct brep_specific*
brep_specific_new()
{
    return (struct brep_specific*)bu_calloc(1, sizeof(struct brep_specific),"brep_specific_new");
}

void
brep_specific_delete(struct brep_specific* bs)
{
    if (bs != NULL) {
	delete bs->bvh;
	bu_free(bs,"brep_specific_delete");
    }
}




//--------------------------------------------------------------------------------
// prep

/**
 * Given a list of face bounding volumes for an entire brep, build up
 * an appropriate hierarchy to make it efficient (binary may be a
 * reasonable choice, for example).
 */
void
brep_bvh_subdivide(BBNode* parent, const std::list<SurfaceTree*>& face_trees)
{
    // XXX this needs to handle a threshold and some reasonable space
    // partitioning
    //     for (BVList::const_iterator i = face_bvs.begin(); i != face_bvs.end(); i++) {
    // 	parent->gs_insert(*i);
    //     }
    for (std::list<SurfaceTree*>::const_iterator i = face_trees.begin(); i != face_trees.end(); i++) {
	parent->addChild((*i)->getRootNode());
    }
}

inline void
distribute(const int count, const ON_3dVector* v, double x[], double y[], double z[])
{
    for (int i = 0; i < count; i++) {
	x[i] = v[i].x;
	y[i] = v[i].y;
	z[i] = v[i].z;
    }
}

bool
brep_pt_trimmed(pt2d_t pt, const ON_BrepFace& face) {
    bool retVal = false;
    TRACE1("brep_pt_trimmed: " << PT2(pt));
    // for each loop
    const ON_Surface* surf = face.SurfaceOf();
    double umin, umax;
    ON_2dPoint from, to;
    from.x = pt[0];
    from.y = to.y = pt[1];
    surf->GetDomain(0, &umin, &umax);
    to.x = umax + 1;
    ON_Line ray(from, to);
    int intersections = 0;
    for (int i = 0; i < face.LoopCount(); i++) {
	ON_BrepLoop* loop = face.Loop(i);
	// for each trim
	for (int j = 0; j < loop->m_ti.Count(); j++) {
	    ON_BrepTrim& trim = face.Brep()->m_T[loop->m_ti[j]];
	    const ON_Curve* trimCurve = trim.TrimCurveOf();
	    // intersections += brep_count_intersections(ray, trimCurve);
	    //ray.IntersectCurve(trimCurve, intersections, 0.0001);
	    intersections += trimCurve->NumIntersectionsWith(ray);
	}
    }

    /* If we base trimming on the number of intersections with, rhino
     * generated curves won't raytrace.  In fact, we need to ignore
     * trimming for the time being, just return false.
     *
     * FIXME: figure out what this code does, and fix it for rhino
     * generated geometries. djg 4/16/08
     */

    // the point is trimmed if the # of intersections is even and non-zero
    retVal= (intersections > 0 && (intersections % 2) == 0);

    return retVal;
}
#define PLOTTING 1
#if PLOTTING

#include "plot3.h"

static int pcount = 0;
static FILE* plot = NULL;
static FILE*
plot_file()
{
    if (plot == NULL) {
	plot = fopen("out.pl","w");
	point_t min, max;
	VSET(min,-2048,-2048,-2048);
	VSET(max, 2048, 2048, 2048);
	pdv_3space(plot, min, max);
    }
    return plot;
}
static FILE*
plot_file(const char *pname)
{
    if (plot != NULL) 
		(void)fclose(plot_file());
	plot = fopen(pname,"w");
	point_t min, max;
	VSET(min,-2048,-2048,-2048);
	VSET(max, 2048, 2048, 2048);
	pdv_3space(plot, min, max);
    
    return plot;
}

#define BLUEVIOLET 138,43,226
#define CADETBLUE 95,159,159
#define CORNFLOWERBLUE 66,66,111
#define LIGHTBLUE 173,216,230
#define DARKGREEN 0,100,0
#define KHAKI 189,183,107
#define FORESTGREEN 34,139,34
#define LIMEGREEN 124,252,0
#define PALEGREEN 152,251,152
#define DARKORANGE 255,140,0
#define DARKSALMON 233,150,122
#define LIGHTCORAL 240,128,128
#define PEACH 255,218,185
#define DEEPPINK 255,20,147
#define HOTPINK 255,105,180
#define INDIANRED 205,92,92
#define DARKVIOLET 148,0,211
#define MAROON 139,28,98
#define GOLDENROD 218,165,32
#define DARKGOLDENROD 184,134,11
#define LIGHTGOLDENROD 238,221,130
#define DARKYELLOW 155,155,52
#define LIGHTYELLOW 255,255,224
#define RED 255,0,0
#define GREEN 0,255,0
#define BLUE 0,0,255
#define YELLOW 255,255,0
#define MAGENTA 255,0,255
#define CYAN 0,255,255
#define BLACK 0,0,0
#define WHITE 255,255,255

#define M_COLOR_PLOT(c) pl_color(plot_file(), c)
#define COLOR_PLOT(r, g, b) pl_color(plot_file(),(r),(g),(b))
#define M_PT_PLOT(p) 	{ 		\
    point_t pp,ppp;		        \
    vect_t grow;                        \
    VSETALL(grow,0.01);                  \
    VADD2(pp, p, grow);                 \
    VSUB2(ppp, p, grow);                \
    pdv_3box(plot_file(), pp, ppp); 	\
}
#define PT_PLOT(p) 	{ 		\
    point_t 	pp; 			\
    VSCALE(pp, p, 1.001); 		\
    pdv_3box(plot_file(), p, pp); 	\
}
#define LINE_PLOT(p1, p2) pdv_3move(plot_file(), p1); pdv_3line(plot_file(), p1, p2)
#define BB_PLOT(p1, p2) pdv_3box(plot_file(), p1, p2)

#endif /* PLOTTING */


void plotsurfaceleafs(SurfaceTree* surf) {
    double min[3],max[3];
    list<BBNode*> leaves;
    surf->getLeaves(leaves);

    for (list<BBNode*>::iterator i = leaves.begin(); i != leaves.end(); i++) {
		SubsurfaceBBNode* bb = dynamic_cast<SubsurfaceBBNode*>(*i);
		if (bb->m_trimmed) {
			COLOR_PLOT(255, 0, 0);
		} else if (bb->m_checkTrim) {
			COLOR_PLOT(0, 0, 255); 
		} else {
			COLOR_PLOT(255, 0, 255); 
		}
		/*
		if (bb->m_xgrow) {
			M_COLOR_PLOT(RED); 
		} else if (bb->m_ygrow) {
			M_COLOR_PLOT(GREEN); 
		} else if (bb->m_zgrow) {
			M_COLOR_PLOT(BLUE); 
		} else {
			COLOR_PLOT(100, 100, 100); 
		}
		*/
		if ((!bb->m_trimmed) && (!bb->m_checkTrim)) {
		if (false) {
			bb->GetBBox(min,max);
		} else {
		    //VSET(min,bb->m_u[0]+0.001,bb->m_v[0]+0.001,0.0);
		    //VSET(max,bb->m_u[1]-0.001,bb->m_v[1]-0.001,0.0);
			VSET(min,bb->m_u[0],bb->m_v[0],0.0);
			VSET(max,bb->m_u[1],bb->m_v[1],0.0);
		}
		BB_PLOT(min,max);
		}
    }
    return;
}
void plotleaf3d(SubsurfaceBBNode* bb) {
    double min[3],max[3];
	double u,v;

	if (bb->m_trimmed) {
		COLOR_PLOT(255, 0, 0);
	} else if (bb->m_checkTrim) {
		COLOR_PLOT(0, 0, 255); 
	} else {
		COLOR_PLOT(255, 0, 255); 
	}
	
	if (true) {
		bb->GetBBox(min,max);
	} else {
		VSET(min,bb->m_u[0]+0.001,bb->m_v[0]+0.001,0.0);
		VSET(max,bb->m_u[1]-0.001,bb->m_v[1]-0.001,0.0);
	}
	BB_PLOT(min,max);
	
	M_COLOR_PLOT(YELLOW);
	bool start=true;
	point_t a,b;
	ON_3dPoint p;
	const ON_BrepFace* f = bb->m_face;
	const ON_Surface* surf = f->SurfaceOf();
	for(u=bb->m_u[0];u<=bb->m_u[1];u=u+(bb->m_u[1] - bb->m_u[0])/100) {
		for(v=bb->m_v[0];v<=bb->m_v[1];v=v+(bb->m_v[1] - bb->m_v[0])/100) {
			if (start) {
				start=false;
				p = surf->PointAt(u,v);
				VMOVE(b,p);
			} else {
				VMOVE(a,b);
				p = surf->PointAt(u,v);
				VMOVE(b,p);
				LINE_PLOT(a,b);
			}
		}
	}
	
    return;
}
void plotleafuv(SubsurfaceBBNode* bb) {
    double min[3],max[3];

	if (bb->m_trimmed) {
		COLOR_PLOT(255, 0, 0);
	} else if (bb->m_checkTrim) {
		COLOR_PLOT(0, 0, 255); 
	} else {
		COLOR_PLOT(255, 0, 255); 
	}
	
	if (false) {
		bb->GetBBox(min,max);
	} else {
		VSET(min,bb->m_u[0]+0.001,bb->m_v[0]+0.001,0.0);
		VSET(max,bb->m_u[1]-0.001,bb->m_v[1]-0.001,0.0);
	}
	BB_PLOT(min,max);
	
    return;
}

void plottrim(ON_BrepFace &face ) {
    const ON_Surface* surf = face.SurfaceOf();
    double umin, umax;
    double pt1[3],pt2[3];
    ON_2dPoint from, to;
    COLOR_PLOT( 0, 255, 255); 
    surf->GetDomain(0, &umin, &umax);
    for (int i = 0; i < face.LoopCount(); i++) {
	ON_BrepLoop* loop = face.Loop(i);
	// for each trim
	for (int j = 0; j < loop->m_ti.Count(); j++) {
	    ON_BrepTrim& trim = face.Brep()->m_T[loop->m_ti[j]];
	    const ON_Curve* trimCurve = trim.TrimCurveOf();

	if (trimCurve->IsLinear()) {
	/*
	    ON_BrepVertex& v1 = face.Brep()->m_V[trim.m_vi[0]];
	    ON_BrepVertex& v2 = face.Brep()->m_V[trim.m_vi[1]];
	    VMOVE(pt1, v1.Point());
	    VMOVE(pt2, v2.Point());
	    LINE_PLOT(pt1,pt2);
		*/

		int knotcnt = trimCurve->SpanCount();
		double *knots = new double[knotcnt+1];

		trimCurve->GetSpanVector(knots);
		for(int i=1;i<=knotcnt;i++) {
			ON_3dPoint p = trimCurve->PointAt(knots[i-1]);
			VMOVE(pt1, p);
			p = trimCurve->PointAt(knots[i]);
			VMOVE(pt2, p);
			LINE_PLOT(pt1,pt2);
		}

	} else {
	    ON_Interval dom = trimCurve->Domain();
	    // XXX todo: dynamically sample the curve
	    for (int i = 0; i <= 10000; i++) {
		ON_3dPoint p = trimCurve->PointAt(dom.ParameterAt((double)i/10000.0));
		VMOVE(pt2, p);
		if (true) { //(i != 0) {
		    LINE_PLOT(pt1,pt2);
		} 
		VMOVE(pt1,p);
	    }
	}

	}
    }
    return;
}
void plottrim(const ON_Curve &curve, double from, double to ) {
    point_t pt1,pt2;
    // XXX todo: dynamically sample the curve
    for (int i = 0; i <= 10000; i++) {
	ON_3dPoint p = curve.PointAt(from + (to-from)*(double)i/10000.0);//dom.ParameterAt((double)i/10000.0));
	VMOVE(pt2, p);
	if (i != 0) {
	    LINE_PLOT(pt1,pt2);
	} 
	VMOVE(pt1,p);
    }
}
void plottrim(ON_Curve &curve ) {
    point_t pt1,pt2;
    // XXX todo: dynamically sample the curve
    ON_Interval dom = curve.Domain();
    for (int i = 0; i <= 10000; i++) {
	ON_3dPoint p = curve.PointAt(dom.ParameterAt((double)i/10000.0));
	VMOVE(pt2, p);
	if (i != 0) {
	    LINE_PLOT(pt1,pt2);
	} 
	VMOVE(pt1,p);
    }
}

double
getVerticalTangent(const ON_Curve *curve,double min,double max) {
    double mid;
    ON_3dVector tangent;
    bool tanmin;

    tangent = curve->TangentAt(min);
    tanmin = (tangent[X] < 0.0);
    while ( (max-min) > 0.00001 ) {
	mid = (max + min)/2.0;
	tangent = curve->TangentAt(mid);
	if (NEAR_ZERO(tangent[X], 0.00001)) {
	    return mid;
	}
	if ( (tangent[X] < 0.0) == tanmin ) {
	    min = mid;
	} else {
	    max = mid;
	}
    }
    return min;
}

double
getHorizontalTangent(const ON_Curve *curve,double min,double max) {
    double mid;
    ON_3dVector tangent;
    bool tanmin;

    tangent = curve->TangentAt(min);
    tanmin = (tangent[Y] < 0.0);
    while ( (max-min) > 0.00001 ) {
	mid = (max + min)/2.0;
	tangent = curve->TangentAt(mid);
	if (NEAR_ZERO(tangent[Y], 0.00001)) {
	    return mid;
	}
	if ( (tangent[Y] < 0.0) == tanmin ) {
	    min = mid;
	} else {
	    max = mid;
	}
    }
    return min;
}

bool
split_trims_hv_tangent(const ON_Curve* curve, ON_Interval& t, list<double>& list) {
    bool tanx1,tanx2,tanx_changed;
    bool tany1,tany2,tany_changed;
    bool tan_changed;
    ON_3dVector tangent1,tangent2;
    ON_3dPoint p1,p2;

    tangent1 = curve->TangentAt(t[0]);
    tangent2 = curve->TangentAt(t[1]);
    
    tanx1 = (tangent1[X] < 0.0);
    tanx2 = (tangent2[X] < 0.0);
    tany1 = (tangent1[Y] < 0.0);
    tany2 = (tangent2[Y] < 0.0);

    tanx_changed =(tanx1 != tanx2);
    tany_changed =(tany1 != tany2);

    tan_changed = tanx_changed || tany_changed;
    
    if ( tan_changed ) {
	if (tanx_changed && tany_changed) {//horz & vert simply split
#if 1
	    double midpoint = (t[1]+t[0])/2.0;
	    ON_Interval left(t[0],midpoint);
	    ON_Interval right(midpoint,t[1]);
	    split_trims_hv_tangent(curve, left, list);
	    split_trims_hv_tangent(curve, right, list);
	    return true;
#else
	    M_COLOR_PLOT( RED );
#endif
	} else if (tanx_changed) {//find horz
#if 1
	    double x = getVerticalTangent(curve,t[0],t[1]);
/*
	    point_t p;

	    p1 = curve->PointAt(x);
	    M_COLOR_PLOT( RED );
	    VMOVE(p,p1);
	    M_PT_PLOT(p);
*/
	    M_COLOR_PLOT( DARKORANGE );
	    list.push_back(x);
#else	    
	    M_COLOR_PLOT( DARKORANGE );
#endif
	} else { //find vert
#if 1
	    double x = getHorizontalTangent(curve,t[0],t[1]);
/*
	    point_t p;

	    p1 = curve->PointAt(x);
	    M_COLOR_PLOT( RED );
	    VMOVE(p,p1);
	    M_PT_PLOT(p);
*/
	    M_COLOR_PLOT( MAGENTA );
	    list.push_back(x);
#else
	    M_COLOR_PLOT( MAGENTA );
#endif
	}
    } else { // check point slope for change
	bool slopex,slopex_changed;
	bool slopey,slopey_changed;
	bool slope_changed;
	
	p1 = curve->PointAt(t[0]);
	p2 = curve->PointAt(t[1]);
	
	slopex = ((p2[X] - p1[X]) < 0.0);
	slopey = ((p2[Y] - p1[Y]) < 0.0);
	
	slopex_changed = (slopex != tanx1);
	slopey_changed = (slopey != tany1);

	slope_changed = slopex_changed || slopey_changed;
			
	if (slope_changed) {  //2 horz or 2 vert changes simply split
#if 1
	    double midpoint = (t[1]+t[0])/2.0;
	    ON_Interval left(t[0],midpoint);
	    ON_Interval right(midpoint,t[1]);
	    split_trims_hv_tangent(curve, left, list);
	    split_trims_hv_tangent(curve, right, list);
	    return true;
#else
	    M_COLOR_PLOT( BLUE );
#endif
	} else {
	    M_COLOR_PLOT( DARKGREEN );
	}
    }
    //plot color coded segment
    plottrim(*curve,t[0],t[1]);

    return true;
}

/* XXX - most of this function is broken :-(except for the bezier span
 * caching need to fix it! - could provide real performance
 * benefits...
 */
void
brep_preprocess_trims(ON_BrepFace& face, SurfaceTree* tree) {
    double min[3],max[3];
	
//#define KCONTROLPNTS
#ifdef KCONTROLPNTS
	const ON_Surface* surf = face.SurfaceOf();
	int uknotcnt = surf->SpanCount(0);
	bu_log("Surf SpanCount(0): %d\n",uknotcnt);
	int vknotcnt = surf->SpanCount(1);
	bu_log("Surf SpanCount(1): %d\n",vknotcnt);

	double *uknots = new double[(uknotcnt+1)];
	double *vknots = new double[(vknotcnt+1)];

	surf->GetSpanVector(0,uknots);
	surf->GetSpanVector(1,vknots);

	ON_3dPoint p; // = surf->PointAt(uknots[i],vknots[j]);
	ON_3dVector n;
	double u;
	double v;
	point_t point;
	point_t end;
	vect_t dir;
	point_t prevpoint;
	vect_t prevdir;
	vect_t diff;

	u = uknots[0];
	v = vknots[0];
	if (surf->EvNormal( u, v, p, n )) {
	    M_COLOR_PLOT(MAGENTA);
	    VMOVE(prevpoint,p);
	    VMOVE(prevdir,n);
	    VSCALE(end,prevdir,1.0);
	    VADD2(prevpoint,prevpoint,end);
	    VADD2(end,prevpoint,end);
	    M_PT_PLOT(prevpoint);
	    LINE_PLOT(prevpoint,end);
	}
	u = uknots[1];
	v = vknots[0];
	if (surf->EvNormal( u, v, p, n )) {
	    M_COLOR_PLOT(WHITE);
	    VMOVE(prevpoint,p);
	    VMOVE(prevdir,n);
	    VSCALE(end,prevdir,1.0);
	    VADD2(prevpoint,prevpoint,end);
	    VADD2(end,prevpoint,end);
	    M_PT_PLOT(prevpoint);
	    LINE_PLOT(prevpoint,end);
	}
	bool pxdir,pydir,pzdir;
	bool xdir,ydir,zdir;
	vect_t cp;
	fastf_t dp;
	    for(int j=0;j<=vknotcnt;j++) {
	for(int i=0;i<=uknotcnt;i++) {
		if ((i==0)&&(j==0)) {
		    VMOVE(point,prevpoint);
		    VMOVE(dir,prevdir)
		    pxdir = pydir = pzdir = true;
		} else {
		    u = uknots[i];
		    v = vknots[j];
		    if (surf->EvNormal( u, v, p, n )) {
			VMOVE(point,p);
			VMOVE(dir,n);
		    }
		}
		VCROSS(cp,dir,prevdir);
		dp = VDOT(dir,prevdir);
		//bu_log( "VCROSS <%g,%g,%g>\n",cp[X],cp[Y],cp[Z] );
		//bu_log( "VDOT  - %g\n",dp );
		VSUB2(diff,dir,prevdir);
		xdir = !(diff[0] < 0.0);
		ydir = !(diff[1] < 0.0);
		zdir = !(diff[2] < 0.0);
		VSCALE(end,dir,dp);
		VADD2(end,point,end);
		if (xdir!=pxdir){
		    M_COLOR_PLOT(RED);
		} else if (pydir!=ydir){
		    M_COLOR_PLOT(GREEN);
		} else if (pzdir!=zdir){
		    M_COLOR_PLOT(BLUE);
		} else {
		    M_COLOR_PLOT(YELLOW);
		}
		pxdir = xdir;
		pydir = ydir;
		pzdir = zdir;
		VMOVE(prevdir,dir);
		VMOVE(prevpoint,point);
		M_PT_PLOT(point);
		LINE_PLOT(point,end);
	    }
	}

	
#endif
	CurveTree* ct = new CurveTree(&face);
//#define KDISCONTS
#ifdef KDISCONTS
	for (int i = 0; i < face.LoopCount(); i++) {
	    bool innerLoop = (i > 0) ? true : false;
	    ON_BrepLoop* loop = face.Loop(i);
	    // for each trim
	    for (int j = 0; j < loop->m_ti.Count(); j++) {
		ON_BrepTrim& trim = face.Brep()->m_T[loop->m_ti[j]];
		const ON_Curve* trimCurve = trim.TrimCurveOf();
		double min,max;
		(void)trimCurve->GetDomain(&min, &max);
		ON_Interval t(min,max);
		double t_dis;
		int hint=0;
		int dtype=0;
		double cos_angle_tolerance=0.0000001;
		double curvature_tolerance=0.0000001;
		ON_3dPoint p1,p2,p3;
		vect_t grow;
		VSETALL(grow,0.1); // grow the box a bit
		int knotcnt = trimCurve->SpanCount();
		double *knots = new double[knotcnt+1];
		trimCurve->GetSpanVector(knots);
		ON_3dVector tangent1,tangent2;

		bool tanx1,tanx2,tanx_changed;
		bool tany1,tany2,tany_changed;
		bool tan_changed;
		for(int i=0;i<=knotcnt;i++) {
			point_t p;

			p1 = trimCurve->PointAt(knots[i]);
			if (i == 0) {
			M_COLOR_PLOT( RED );
			} else if (i == knotcnt) {
			M_COLOR_PLOT( GREEN );
			} else {
			M_COLOR_PLOT( YELLOW );
			}
			VMOVE(p,p1);
			M_PT_PLOT(p);
		}
#if 0
		for(int i=1;i<=knotcnt;i++) {
		    list<double> splitlist;
		    ON_Interval t(knots[i-1],knots[i]);

		    //split_trims_hv_tangent(trimCurve,t,splitlist);
		    for( list<double>::iterator l=splitlist.begin();l != splitlist.end();l++) {
			double x = *l;
			point_t p;

			p1 = trimCurve->PointAt(x);
			M_COLOR_PLOT( RED );
			VMOVE(p,p1);
			M_PT_PLOT(p);
		    }
		}
		for(int i=0;i<=knotcnt;i++) {
		    p1 = trimCurve->PointAt(knots[i]);
		    M_COLOR_PLOT(HOTPINK);
		    M_PT_PLOT(p1);
		}
#endif
/*
		while ( trimCurve->GetNextDiscontinuity( 
			    ON::G2_continuous,
			    min,max,&t_dis,
			    &hint,&dtype) ) {

		    COLOR_PLOT( 255, 255, 255 );
		    p1 = trimCurve->PointAt(t_dis);
		    VADD2(p2, p1, grow);
		    VSUB2(p1, p1, grow);
		    BB_PLOT(p1,p2);
		    min=t_dis;
		}
*/
	    }
	}		

#endif
//#define KCURVELETS
#ifdef KCURVELETS
		list<BRNode*> curvelets;
		ct->getLeaves(curvelets);
		//ON_Interval u(-5.4375,-4.53125);
		//ON_Interval v(-5.4375,-4.53125);
		ct->getLeaves(curvelets);
		
		for (list<BRNode*>::iterator i = curvelets.begin(); i != curvelets.end(); i++) {
			SubcurveBRNode* br = dynamic_cast<SubcurveBRNode*>(*i);
			if (br->m_XIncreasing) {
				COLOR_PLOT( 255, 255, 0 );
			} else {
				COLOR_PLOT( 0, 255, 0 );
			}
			br->GetBBox(min,max);
			BB_PLOT(min,max);
		}
		curvelets.clear();
#endif
	
    list<BBNode*> leaves;
    tree->getLeaves(leaves);
	
    for (list<BBNode*>::iterator i = leaves.begin(); i != leaves.end(); i++) {
		SubsurfaceBBNode* bb = dynamic_cast<SubsurfaceBBNode*>(*i);
		bb->prepTrims(ct);
		/*
	// check to see if the bbox encloses a trim
	ON_3dPoint uvmin(bb->m_u.Min(), bb->m_v.Min(), 0);
	ON_3dPoint uvmax(bb->m_u.Max(), bb->m_v.Max(), 0);
	ON_BoundingBox bbox(uvmin, uvmax);
	for (int i = 0; i < face.Brep()->m_L.Count(); i++) {
	    ON_BrepLoop& loop = face.Brep()->m_L[i];
	    // for each trim
	    for (int j = 0; j < loop.m_ti.Count(); j++) {
		ON_BrepTrim& trim = face.Brep()->m_T[loop.m_ti[j]];
*/
		/* tell the NURBS curves to cache their Bezier spans
		 * (used in trimming routines, and not thread safe)
		 */
/*
		const ON_Curve* c = trim.TrimCurveOf();
		if (c->ClassId()->IsDerivedFrom(&ON_NurbsCurve::m_ON_NurbsCurve_class_id)) {
		    ON_NurbsCurve::Cast(c)->CacheBezierSpans();
		}
	    }
	}
*/
	//bb->m_checkTrim = true; // XXX - ack, hardcode for now

	/* for this node to be completely trimmed, all four corners
	 * must be trimmed and the depth of the tree needs to be > 0,
	 * since 0 means there is just a single leaf - and since
	 * "internal" outer loops will be make a single node seem
	 * trimmed, we must account for it.
	 */
	//bb->m_trimmed = false; // XXX - ack, hardcode for now
    }
}

int
brep_build_bvh(struct brep_specific* bs, struct rt_brep_internal* bi)
{
    ON_TextLog tl(stderr);
    ON_Brep* brep = bs->brep;
    if (brep == NULL || !brep->IsValid(&tl)) {
		bu_log("brep is NOT valid");
		return -1;
    }
	
    bs->bvh = new BBNode(brep->BoundingBox());
	
    /* need to extract faces, and build bounding boxes for each face,
		* then combine the face BBs back up, combining them together to
		* better split the hierarchy
		*/
    std::list<SurfaceTree*> surface_trees;
    ON_BrepFaceArray& faces = brep->m_F;
    for (int i = 0; i < faces.Count(); i++) {
		surface_trees.clear();
        TRACE1("Face: " << i);
		bu_log("Prepping Face: %d of %d\n", i+1, faces.Count());
		ON_BrepFace& face = faces[i];
//#define KPLOT
#ifdef KPLOT // debugging hacks to look at specific faces
		if (i == 196) { //(i == 0)) { // && ((i <= 6) ||(i >= 5))) {
	char buffer[80];
	sprintf(buffer,"Face%d.pl",i+1);
	plot_file((const char *)buffer);
#endif
		SurfaceTree* st = new SurfaceTree(&face);
		face.m_face_user.p = st;
		brep_preprocess_trims(face, st);
		/* add the surface bounding volumes to a list, so we can build
		* down a hierarchy from the brep bounding volume
		*/
		surface_trees.push_back(st);
#ifdef KPLOT // debugging hacks to look at specific faces
			
			if (true) { //plotting utah_brep_intersecthacks i==0) {
			    plottrim(face);
			    plotsurfaceleafs(st);
			}
			
		}
#endif
		brep_bvh_subdivide(bs->bvh, surface_trees);
	}
#ifdef KPLOT // debugging hacks to look at specific faces
    (void)fclose(plot_file());
#endif
    return 0;
}

void
brep_calculate_cdbitems(struct brep_specific* bs, struct rt_brep_internal* bi)
{

}


/********************************************************************************
 * BRL-CAD Primitive interface
 ********************************************************************************/
/**
 *   			R T _ B R E P _ P R E P
 *
 *  Given a pointer of a GED database record, and a transformation
 *  matrix, determine if this is a valid NURB, and if so, prepare the
 *  surface so the intersections will work.
 */
int
rt_brep_prep(struct soltab *stp, struct rt_db_internal* ip, struct rt_i* rtip)
{
    TRACE1("rt_brep_prep");
    /* This prepares the NURBS specific data structures to be used
     * during intersection... i.e. acceleration data structures and
     * whatever else is needed.
     *
     * Abert's paper (Direct and Fast Ray Tracing of NURBS Surfaces)
     * suggests using a bounding volume hierarchy (instead of KD-tree)
     * and building it down to a satisfactory flatness criterion
     * (which they do not give information about).
     */
    struct rt_brep_internal* bi;
    struct brep_specific* bs;

    RT_CK_DB_INTERNAL(ip);
    bi = (struct rt_brep_internal*)ip->idb_ptr;
    RT_BREP_CK_MAGIC(bi);

    if ((bs = (struct brep_specific*)stp->st_specific) == NULL) {
	bs = (struct brep_specific*)bu_malloc(sizeof(struct brep_specific), "brep_specific");
	bs->brep = bi->brep;
	bi->brep = NULL;
	stp->st_specific = (genptr_t)bs;
    }

    if (brep_build_bvh(bs, bi) < 0) {
	return -1;
    }

    point_t adjust;
    VSETALL(adjust, 1);
    bs->bvh->GetBBox(stp->st_min, stp->st_max);
    // expand outer bounding box...
    VSUB2(stp->st_min, stp->st_min, adjust);
    VADD2(stp->st_max, stp->st_max, adjust);
    VADD2SCALE(stp->st_center, stp->st_min, stp->st_max, 0.5);
    vect_t work;
    VSUB2SCALE(work, stp->st_max, stp->st_min, 0.5);
    fastf_t f = work[X];
    V_MAX(f, work[Y]);
    V_MAX(f, work[Z]);
    stp->st_aradius = f;
    stp->st_bradius = MAGNITUDE(work);

    brep_calculate_cdbitems(bs, bi);


    return 0;
}


/**
 * R T _ B R E P _ P R I N T
 */
void
rt_brep_print(register const struct soltab *stp)
{
}

//================================================================================
// shot support

class plane_ray {
public:
    vect_t n1;
    fastf_t d1;

    vect_t n2;
    fastf_t d2;
};


// void
// brep_intersect_bv(IsectList& inters, struct xray* r, BoundingVolume* bv)
// {
//     fastf_t tnear, tfar;
//     bool intersects = bv->intersected_by(r,&tnear,&tfar);
//     if (intersects && bv->is_leaf() && !dynamic_cast<SurfaceBV*>(bv)->isTrimmed()) {
// 	inters.push_back(IRecord(bv, tnear));
//     } else if (intersects)
// 	for (BVList::iterator i = bv->children.begin(); i != bv->children.end(); i++) {
// 	    brep_intersect_bv(inters, r, *i);
// 	}
// }


void
brep_get_plane_ray(ON_Ray& r, plane_ray& pr)
{
    vect_t v1;
    VMOVE(v1, r.m_dir);
    fastf_t min = MAX_FASTF;
    int index = -1;
    for (int i = 0; i < 3; i++) {
	// find the smallest component
	if (fabs(v1[i]) < min) {
	    min = fabs(v1[i]);
	    index = i;
	}
    }
    v1[index] += 1; // alter the smallest component
    VCROSS(pr.n1, v1, r.m_dir); // n1 is perpendicular to v1
    VUNITIZE(pr.n1);
    VCROSS(pr.n2, pr.n1, r.m_dir);       // n2 is perpendicular to v1 and n1
    VUNITIZE(pr.n2);
    pr.d1 = VDOT(pr.n1, r.m_origin);
    pr.d2 = VDOT(pr.n2, r.m_origin);
    TRACE1("n1:" << ON_PRINT3(pr.n1) << " n2:" << ON_PRINT3(pr.n2) << " d1:" << pr.d1 << " d2:" << pr.d2);
}


class brep_hit {
public:
    const ON_BrepFace& face;
    point_t origin;
    point_t point;
    vect_t  normal;
    pt2d_t  uv;
    bool    trimmed;
    bool    closeToEdge;
    bool    oob;
    // XXX - calculate the dot of the dir with the normal here!
    SubsurfaceBBNode const * sbv;

    brep_hit(const ON_BrepFace& f, const point_t orig, const point_t p, const vect_t n, const pt2d_t _uv)
	: face(f), trimmed(false), closeToEdge(false), oob(false), sbv(NULL)
    {
	VMOVE(origin, orig);
	VMOVE(point, p);
	VMOVE(normal, n);
	move(uv, _uv);
    }

    brep_hit(const brep_hit& h)
	: face(h.face), trimmed(h.trimmed), closeToEdge(h.closeToEdge), oob(h.oob), sbv(h.sbv)
    {
	VMOVE(origin, h.origin);
	VMOVE(point, h.point);
	VMOVE(normal, h.normal);
	move(uv, h.uv);
    }

    brep_hit& operator=(const brep_hit& h)
    {
	const_cast<ON_BrepFace&>(face) = h.face;
	VMOVE(origin, h.origin);
	VMOVE(point, h.point);
	VMOVE(normal, h.normal);
	move(uv, h.uv);
	trimmed = h.trimmed;
	closeToEdge = h.closeToEdge;
	oob = h.oob;
	sbv = h.sbv;

        return *this;
    }

    bool operator==(const brep_hit& h)
    {
	return NEAR_ZERO(DIST_PT_PT(point, h.point), BREP_SAME_POINT_TOLERANCE);
    }

    bool operator<(const brep_hit& h)
    {
	return DIST_PT_PT(point, origin) < DIST_PT_PT(h.point, origin);
    }
};
typedef std::list<brep_hit> HitList;


// class HitSorter : public std::less<brep_hit>
// {
//     point_t origin;
// public:
//     HitSorter(point_t o) {
// 	VMOVE(origin, o);
//     }

//     bool operator()(const brep_hit& left, const brep_hit& right) {
// 	return DIST_PT_PT(left.point, origin) < DIST_PT_PT(right.point, origin);
//     }
// };


void
brep_r(const ON_Surface* surf, plane_ray& pr, pt2d_t uv, ON_3dPoint& pt, ON_3dVector& su, ON_3dVector& sv, pt2d_t R)
{
    assert(surf->Ev1Der(uv[0], uv[1], pt, su, sv));
    R[0] = VDOT(pr.n1,((fastf_t*)pt)) - pr.d1;
    R[1] = VDOT(pr.n2,((fastf_t*)pt)) - pr.d2;
}


void
brep_newton_iterate(const ON_Surface* surf, plane_ray& pr, pt2d_t R, ON_3dVector& su, ON_3dVector& sv, pt2d_t uv, pt2d_t out_uv)
{
    mat2d_t jacob = { VDOT(pr.n1,((fastf_t*)su)), VDOT(pr.n1,((fastf_t*)sv)),
		      VDOT(pr.n2,((fastf_t*)su)), VDOT(pr.n2,((fastf_t*)sv)) };
    mat2d_t inv_jacob;
    if (mat2d_inverse(inv_jacob, jacob)) {
	// check inverse validity
	pt2d_t tmp;
	mat2d_pt2d_mul(tmp, inv_jacob, R);
	pt2dsub(out_uv, uv, tmp);
    } else {
	TRACE2("inverse failed"); // XXX how to handle this?
	move(out_uv, uv);
    }
}


typedef enum {
    BREP_INTERSECT_RIGHT_OF_EDGE = -5,
    BREP_INTERSECT_MISSED_EDGE = -4,
    BREP_INTERSECT_ROOT_ITERATION_LIMIT = -3,
    BREP_INTERSECT_ROOT_DIVERGED = -2,
    BREP_INTERSECT_OOB = -1,
    BREP_INTERSECT_TRIMMED = 0,
    BREP_INTERSECT_FOUND = 1
} brep_intersect_reason_t;

static const char* BREP_INTERSECT_REASON(brep_intersect_reason_t index)
{
    static const char *reason[] = {
	"grazed to the right of the edge",
	"missed the edge altogether (outside tolerance)",
	"hit root iteration limit",
	"root diverged",
	"out of subsurface bounds",
	"trimmed",
	"found",
	"UNKNOWN"
    };

    return reason[index+5];
}


int
brep_edge_check(int reason,
		const SubsurfaceBBNode* sbv,
		const ON_BrepFace* face,
		const ON_Surface* surf,
		const ON_Ray& r,
		HitList& hits)
{
    /* if the intersection was not found for any reason, we need to
     * check and see if we are close to any topological edges; we may
     * have hit a crack...
     *
     * the proper way to do this is to only look at edges
     * interesecting with the subsurface bounding box... but for now,
     * we'll look at the edges associated with the face for the
     * bounding box...
     */

     // XXX - optimize this

    set<ON_BrepEdge*> edges;
    ON_3dPoint pt;
    for (int i = 0; i < face->LoopCount(); i++) {
	ON_BrepLoop* loop = face->Loop(i);
	for (int j = 0; j < loop->TrimCount(); j++) {
	    ON_BrepTrim* trim = loop->Trim(j);
	    ON_BrepEdge* edge = trim->Edge();
	    pair<set<ON_BrepEdge*>::iterator, bool> res = edges.insert(edge);
	    //	    if (res.second) {
	    // only check if its the first time we've seen this edge
	    const ON_Curve* curve = edge->EdgeCurveOf();
	    Sample s;
	    if (curve->CloseTo(ON_3dPoint(hits.back().point), BREP_EDGE_MISS_TOLERANCE, s)) {
		TRACE1("CLOSE TO EDGE");
		hits.back().closeToEdge = true;
		return BREP_INTERSECT_FOUND;
	    }
	}
    }
    return BREP_INTERSECT_TRIMMED;
}


void
utah_F(const ON_3dPoint &S, const ON_3dVector &p1, const double p1d, const ON_3dVector &p2, const double p2d, double &f1, double &f2)
{
    f1 = (S * p1) + p1d;
    f2 = (S * p2) + p2d;
}

void
utah_Fu(const ON_3dVector &Su, const ON_3dVector &p1, const ON_3dVector &p2, double &d0, double &d1)
{
    d0 = Su * p1;
    d1 = Su * p2;
}

void
utah_Fv(const ON_3dVector &Sv, const ON_3dVector &p1, const ON_3dVector &p2, double &d0, double &d1)
{
    d0 = Sv * p1;
    d1 = Sv * p2;
}

void
utah_ray_planes(const ON_Ray &r, ON_3dVector &p1, double &p1d, ON_3dVector &p2, double &p2d)
{
    ON_3dPoint ro(r.m_origin);
    ON_3dVector rdir(r.m_dir);
    double rdx, rdy, rdz;
    double rdxmag, rdymag, rdzmag;

    rdx = rdir.x;
    rdy = rdir.y;
    rdz = rdir.z;

    rdxmag = fabs(rdx);
    rdymag = fabs(rdy);
    rdzmag = fabs(rdz);

    if (rdxmag > rdymag && rdxmag > rdzmag)
	p1 = ON_3dVector(rdy, -rdx, 0);
    else
	p1 = ON_3dVector(0, rdz, -rdy);
	// keith test
	p1.Unitize();
	//keith
    p2 = ON_CrossProduct(p1, rdir);

    p1d = -(p1 * ro);
    p2d = -(p2 * ro);
}

double
utah_calc_t(const ON_Ray &r, ON_3dPoint &S)
{
    ON_3dVector d(r.m_dir);
    ON_3dVector oS(S - r.m_origin);

    return (d * oS) / (d * d);
}

void
utah_pushBack(const ON_Surface* surf, ON_2dPoint &uv)
{
    double t0, t1;

    surf->GetDomain(0, &t0, &t1);
    if (uv.x < t0) {
        uv.x = t0;
    } else if (uv.x >= t1) {
        uv.x = t1 - ROOT_TOL;
    }

    surf->GetDomain(1, &t0, &t1);
    if (uv.y < t0) {
        uv.y = t0;
    } else if (uv.y >= t1) {
        uv.y = t1 - ROOT_TOL;
    }
}



int
utah_newton_4corner_solver(const SubsurfaceBBNode* sbv, const ON_Surface* surf, const ON_Ray& r, ON_2dPoint* ouv, double* t, ON_3dVector* N, bool& converged, int docorners)
{
    int i;
	int intersects = 0;
    double j11, j12, j21, j22;
    double f, g;
    double rootdist, oldrootdist;
    double J, invdetJ;
	double du,dv;

    ON_3dVector p1, p2;
    double p1d = 0, p2d = 0;
    converged = false;
    utah_ray_planes(r, p1, p1d, p2, p2d);

    ON_3dPoint S;
    ON_3dVector Su, Sv;
    ON_2dPoint uv;

    if (docorners) {    
	for( int iu = 0; iu < 2; iu++) {
		for( int iv = 0; iv < 2; iv++) {

			uv.x = sbv->m_u[iu];
			uv.y = sbv->m_v[iv];
			
			ON_2dPoint uv0(uv);
			surf->Ev1Der(uv.x, uv.y, S, Su, Sv);
			
			utah_F(S, p1, p1d, p2, p2d, f, g);
			rootdist = fabs(f) + fabs(g);
			
			for (i = 0; i < BREP_MAX_ITERATIONS; i++) {
				utah_Fu(Su, p1, p2, j11, j21);
				utah_Fv(Sv, p1, p2, j12, j22);
				
				J = (j11 * j22 - j12 * j21);
				
				if (NEAR_ZERO(J, BREP_INTERSECTION_ROOT_EPSILON)) {
					// perform jittered perturbation in parametric domain....
					uv.x = uv.x + .1 * drand48() * (uv0.x - uv.x);
					uv.y = uv.y + .1 * drand48() * (uv0.y - uv.y);
					continue;
				}
				
				invdetJ = 1. / J;
				
				
				du = -invdetJ * (j22 * f - j12 * g);
				dv = -invdetJ * (j11 * g - j21 * f);
				

				if ( i == 0 ) {
					if (((iu == 0) && (du < 0.0)) ||
						((iu==1) && (du > 0.0)))
						break; //head out of U bounds
					if (((iv == 0) && (dv < 0.0)) ||
						((iv==1) && (dv > 0.0)))
						break; //head out of V bounds
				}
				
				
				uv.x -= invdetJ * (j22 * f - j12 * g);
				uv.y -= invdetJ * (j11 * g - j21 * f);
				
				utah_pushBack(surf, uv);
				
				surf->Ev1Der(uv.x, uv.y, S, Su, Sv);
				utah_F(S, p1, p1d, p2, p2d, f, g);
				oldrootdist = rootdist;
				rootdist = fabs(f) + fabs(g);
				
				if (oldrootdist < rootdist) break;
				
				if (rootdist < ROOT_TOL) {
					if (sbv->m_u.Includes(uv.x) && sbv->m_v.Includes(uv.y)) {
						bool new_point = true;
						for(int j=0;j<intersects;j++) {
							if (NEAR_ZERO(uv.x - ouv[j].x, 0.0001) && NEAR_ZERO(uv.y - ouv[j].y, 0.0001)) {
								new_point = false;
							} 
						}
						if (new_point) {
							//bu_log("New Hit Point:(%f %f %f) uv(%f,%f)\n",S.x,S.y,S.z,uv.x,uv.y);
							t[intersects] = utah_calc_t(r, S);
							N[intersects] = ON_CrossProduct(Su, Sv);
							N[intersects].Unitize();
							ouv[intersects].x = uv.x;
							ouv[intersects].y = uv.y;
							intersects++;
							converged = true;
						}
					} //else {
						//bu_log("OOB Point Hit:(%f %f %f) uv(%f,%f)\n",S.x,S.y,S.z,uv.x,uv.y);
					//}
					break;
				}
			}
		}
	}

        }

	
	if (true) {
		uv.x = sbv->m_u.Mid();
		uv.y = sbv->m_v.Mid();
		
		ON_2dPoint uv0(uv);
		surf->Ev1Der(uv.x, uv.y, S, Su, Sv);
		
		utah_F(S, p1, p1d, p2, p2d, f, g);
		rootdist = fabs(f) + fabs(g);
		
		for (i = 0; i < BREP_MAX_ITERATIONS; i++) {
			utah_Fu(Su, p1, p2, j11, j21);
			utah_Fv(Sv, p1, p2, j12, j22);
			
			J = (j11 * j22 - j12 * j21);
			
			if (NEAR_ZERO(J, BREP_INTERSECTION_ROOT_EPSILON)) {
				// perform jittered perturbation in parametric domain....
				uv.x = uv.x + .1 * drand48() * (uv0.x - uv.x);
				uv.y = uv.y + .1 * drand48() * (uv0.y - uv.y);
				continue;
			}
			
			invdetJ = 1. / J;
			
			/*
			 du = -invdetJ * (j22 * f - j12 * g);
			 dv = -invdetJ * (j11 * g - j21 * f);
			 
			 
			 if ( i == 0 ) {
				 if (((iu == 0) && (du < 0.0)) ||
					 ((iu==1) && (du > 0.0)))
					 break; //head out of U bounds
				 if (((iv == 0) && (dv < 0.0)) ||
					 ((iv==1) && (dv > 0.0)))
					 break; //head out of V bounds
			 }
			 */
			
			uv.x -= invdetJ * (j22 * f - j12 * g);
			uv.y -= invdetJ * (j11 * g - j21 * f);
			
			utah_pushBack(surf, uv);
			
			surf->Ev1Der(uv.x, uv.y, S, Su, Sv);
			utah_F(S, p1, p1d, p2, p2d, f, g);
			oldrootdist = rootdist;
			rootdist = fabs(f) + fabs(g);
			
			if (oldrootdist < rootdist) break;
			
			if (rootdist < ROOT_TOL) {
				if (sbv->m_u.Includes(uv.x) && sbv->m_v.Includes(uv.y)) {
					bool new_point = true;
					for(int j=0;j<intersects;j++) {
						if (NEAR_ZERO(uv.x - ouv[j].x, 0.0001) && NEAR_ZERO(uv.y - ouv[j].y, 0.0001)) {
							new_point = false;
						} 
					}
					if (new_point) {
						//bu_log("New Hit Point:(%f %f %f) uv(%f,%f)\n",S.x,S.y,S.z,uv.x,uv.y);
						t[intersects] = utah_calc_t(r, S);
						N[intersects] = ON_CrossProduct(Su, Sv);
						N[intersects].Unitize();
						ouv[intersects].x = uv.x;
						ouv[intersects].y = uv.y;
						intersects++;
						converged = true;
					}
				} //else {
				  //bu_log("OOB Point Hit:(%f %f %f) uv(%f,%f)\n",S.x,S.y,S.z,uv.x,uv.y);
				  //}
				break;
			}
		}
	}
	return intersects;
}

void
utah_newton_solver(const SubsurfaceBBNode* sbv, const ON_Surface* surf, const ON_Ray& r, ON_2dPoint &uv, double& t, ON_3dVector &N, bool& converged)
{
    int i;
    double j11, j12, j21, j22;
    double f, g;
    double rootdist, oldrootdist;
    double J, invdetJ;

    ON_3dVector p1, p2;
    double p1d = 0, p2d = 0;
    converged = false;
    utah_ray_planes(r, p1, p1d, p2, p2d);

    ON_3dPoint S;
    ON_3dVector Su, Sv;
    ON_2dPoint uv0(uv);

    surf->Ev1Der(uv.x, uv.y, S, Su, Sv);

    utah_F(S, p1, p1d, p2, p2d, f, g);
    rootdist = fabs(f) + fabs(g);

    for (i = 0; i < BREP_MAX_ITERATIONS; i++) {
        utah_Fu(Su, p1, p2, j11, j21);
        utah_Fv(Sv, p1, p2, j12, j22);

        J = (j11 * j22 - j12 * j21);

        if (NEAR_ZERO(J, BREP_INTERSECTION_ROOT_EPSILON)) {
            // perform jittered perturbation in parametric domain....
            uv.x = uv.x + .1 * drand48() * (uv0.x - uv.x);
            uv.y = uv.y + .1 * drand48() * (uv0.y - uv.y);
            continue;
        }

        invdetJ = 1. / J;

        uv.x -= invdetJ * (j22 * f - j12 * g);
        uv.y -= invdetJ * (j11 * g - j21 * f);

        utah_pushBack(surf, uv);

        surf->Ev1Der(uv.x, uv.y, S, Su, Sv);
        utah_F(S, p1, p1d, p2, p2d, f, g);
        oldrootdist = rootdist;
        rootdist = fabs(f) + fabs(g);

        if (oldrootdist < rootdist) return;

        if (rootdist < ROOT_TOL) {
            t = utah_calc_t(r, S);
            converged = true;
            N = ON_CrossProduct(Su, Sv);
            N.Unitize();

            return;
        }
    }
}


bool
lines_intersect(double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4)
{
    double tol = 1e-8;
    double A1 = y2-y1;
    double B1 = x1-x2;
    double C1 = A1*x1+B1*y1;

    double A2 = y4-y3;
    double B2 = x3-x4;
    double C2 = A2*x3+B2*y3;

    double det = A1*B2 - A2*B1;

    if (NEAR_ZERO(det, tol)) {
        return false;
    } else {
        double x = (B2*C1 - B1*C2)/det;
        double y = (A1*C2 - A2*C1)/det;

        if ((x >= min(x1, x2)) && (x <= max(x1, x2)) && (x >= min(x3, x4)) && (x <= max(x3, x4)) && (y >= min(y1, y2)) && (y <= max(y1, y2)) && (y >= min(y3, y4)) && (y <= max(y3, y4))) {
            return true;
        }

        if (NEAR_ZERO(x-x1, tol) && NEAR_ZERO(y-y1, tol)) {
            return true;
        }

        if (NEAR_ZERO(x-x2, tol) && NEAR_ZERO(y-y2, tol)) {
            return true;
        }

        if (NEAR_ZERO(x-x3, tol) && NEAR_ZERO(y-y3, tol)) {
            return true;
        }

        if (NEAR_ZERO(x-x4, tol) && NEAR_ZERO(y-y4, tol)) {
            return true;
        }
    }

    return false;
}


bool
utah_isTrimmed(ON_2dPoint uv, const ON_BrepFace *face) {
    static bool approximationsInit = false;
    static const int MAX_CURVES = 10000;
    static const int MAX_NUMBEROFPOINTS = 1000;

    static bool curveApproximated[MAX_CURVES];
    static ON_3dPoint curveApproximations[MAX_CURVES][MAX_NUMBEROFPOINTS];

    ON_wString curveinfo;
    ON_TextLog log(curveinfo);

    if (!approximationsInit) {
        approximationsInit = true;
        for (int i = 0; i < MAX_CURVES; i++) {
            curveApproximated[i] = false;
        }
    }

    if (face == NULL) {
        return false;
    }
    const ON_Surface* surf = face->SurfaceOf();
    if (surf == NULL) {
        return false;
    }
    TRACE1("utah_isTrimmed: " << uv);
    // for each loop
    for (int li = 0; li < face->LoopCount(); li++) {
        ON_BrepLoop* loop = face->Loop(li);
        if (loop == 0) {
            continue;
        }
        // for each trim
        ON_3dPoint closestPoint;
        ON_3dVector tangent, kappa;
        double currentDistance = -10000.0;;
        ON_3dPoint hitPoint(uv.x, uv.y, 0.0);
        for( int lti = 0; lti < loop->TrimCount(); lti++ ) {
            const ON_BrepTrim* trim = loop->Trim( lti );
            if (0 == trim )
                continue;
            const ON_Curve* trimCurve = face->Brep()->m_C2[trim->m_c2i];
            if (trimCurve == 0) {
                continue;
            }

	    // Uncomment the following to get a look at the summary report
	    // of a given trimming curve
	    /* trimCurve->Dump(log);
	       ON_String cinfo = ON_String(curveinfo);
	       const char *info = cinfo.Array();
	       bu_log("%s\n", info);
	    */
	    
            double closestT;
            bool gotClosest = trimCurve->GetClosestPoint(hitPoint, &closestT);
            if (!gotClosest) {
                // Someone needs to work on GetClosestPoint not to fail
                // It is failing on nurbs curves that aren't rational
                // For now if it fails we will use the approx. approach
                double currentDistance;
                double shortestDistance;
                double t;
                ON_Interval domain = trimCurve->Domain();
                double step = (domain.m_t[1] - domain.m_t[0]) / (double) MAX_NUMBEROFPOINTS;
                if (!curveApproximated[trim->m_c2i]) {
                    curveApproximated[trim->m_c2i] = true;
                    t = domain.m_t[0];
                    for (int i = 0; i < MAX_NUMBEROFPOINTS; i++) {
                        curveApproximations[trim->m_c2i][i] = trimCurve->PointAt(t);
                        t += step;
                    }
                }
                closestT = t = domain.m_t[0];
                closestPoint = curveApproximations[trim->m_c2i][0];
                currentDistance = shortestDistance = closestPoint.DistanceTo(hitPoint);
                for (int i = 0; i < MAX_NUMBEROFPOINTS; i++) {
                    closestPoint = curveApproximations[trim->m_c2i][i];
                    currentDistance = closestPoint.DistanceTo(hitPoint);
                    if (currentDistance < shortestDistance) {
                        closestT = t;
                        shortestDistance = currentDistance;
                    }
                    t += step;
                }
            }
            ON_3dPoint testClosestPoint;
            ON_3dVector testTangent, testKappa;
            double testDistance;
            trimCurve->EvCurvature(closestT, testClosestPoint, testTangent, testKappa);
            testDistance = testClosestPoint.DistanceTo(hitPoint);
            if ((currentDistance < 0.0) || (testDistance < currentDistance))
            {
                closestPoint = testClosestPoint;
                tangent = testTangent;
                kappa = testKappa;
                currentDistance = testDistance;
            }
        }
        if (currentDistance >= 0.0)
        {
            ON_3dVector hitDirection(hitPoint.x-closestPoint.x, hitPoint.y-closestPoint.y, hitPoint.z-closestPoint.z);
            double dot = (hitDirection * kappa);
            //printf("closestT=%lf dot=%lf closestPoint=(%lf, %lf, %lf) hitPoint=(%lf, %lf, %lf) tangent=(%lf, %lf, %lf) kappa=(%lf, %lf, %lf) normal=(%lf, %lf, %lf) hitDirection=(%lf, %lf, %lf)\n", closestT, dot, closestPoint.x, closestPoint.y, closestPoint.z, hitPoint.x, hitPoint.y, hitPoint.z, tangent.x, tangent.y, tangent.z, kappa.x, kappa.y, kappa.z, normal.x, normal.y, normal.z, hitDirection.x, hitDirection.y, hitDirection.z);
            if (((li == 0) && (dot < 0.0)) ||
                ((li > 0) && (dot > 0.0))) {
                return true;
            }
        }
    }
    return false;
}


int
utah_brep_intersect_test(const SubsurfaceBBNode* sbv, const ON_BrepFace* face, const ON_Surface* surf, pt2d_t uv, ON_Ray& ray, HitList& hits)
{
    ON_3dVector N[2];
    bool hit = false;
    double t[2];
    ON_2dPoint ouv[2];
    int found = BREP_INTERSECT_ROOT_DIVERGED;
    bool converged = false;
	int numhits;
    
    ON_3dPoint center_pt;
    ON_3dVector normal;
    surf->EvNormal(sbv->m_u.Mid(), sbv->m_v.Mid(), center_pt, normal);
    
    double grazing_float = normal * ray.m_dir;
    
    if (fabs(grazing_float) < 0.1) {
	numhits = utah_newton_4corner_solver( sbv, surf, ray, ouv, t, N, converged, 1);
    } else {
	numhits = utah_newton_4corner_solver( sbv, surf, ray, ouv, t, N, converged, 0);
    }
//utah_newton_4corner_solver(const SubsurfaceBBNode* sbv, const ON_Surface* surf, const ON_Ray& r, ON_2dPoint* ouv, double* t, ON_3dVector* &N, bool& converged)
    /*
     * DDR.  The utah people are using this t_min which represents the
     * last point hit along the ray to ensure we are looking at points
     * futher down the ray.  I haven't implemented this I'm not sure
     * we need it
     *
     * if (converged && (t > 1.e-2) && (t < t_min) && (!utah_isTrimmed(ouv, face))) hit = true;
     *
     */
    //if (converged && (t > 1.e-2) && (!utah_isTrimmed(ouv, face))) hit = true;
	//if (converged && (t > 1.e-2) && (!((SubsurfaceBBNode*)sbv)->isTrimmed(ouv))) hit = true;

    for(int i=0;i < numhits;i++) {
	
	if (converged && (t[i] > 1.e-2)) {
		if  (!((SubsurfaceBBNode*)sbv)->isTrimmed(ouv[i])) {
			hit = true;
//#define KHITPLOT
#ifdef KHITPLOT
			double min[3],max[3];
			COLOR_PLOT(255, 200, 200);
			VSET(min,ouv[0]-0.01,ouv[1]-0.01,0.0);
			VSET(max,ouv[0]+0.01,ouv[1]+0.01,0.0);
			BB_PLOT(min,max);
		} else {
			double min[3],max[3];
			COLOR_PLOT(200, 255, 200);
			VSET(min,ouv[0]-0.01,ouv[1]-0.01,0.0);
			VSET(max,ouv[0]+0.01,ouv[1]+0.01,0.0);
			BB_PLOT(min,max);
		}
#else
	        }
#endif
	}
	//    if (converged && (t > 1.e-2)) hit = true;

    uv[0] = ouv[i].x;
    uv[1] = ouv[i].y;

    if (hit) {
        ON_3dPoint _pt;
        ON_3dVector _norm(N[i]);
        _pt = ray.m_origin + (ray.m_dir*t[i]);
        if (face->m_bRev) _norm.Reverse();
        hit_count += 1;
        hits.push_back(brep_hit(*face,(const fastf_t*)ray.m_origin,(const fastf_t*)_pt,(const fastf_t*)_norm, uv));
        hits.back().sbv = sbv;
        found = BREP_INTERSECT_FOUND;
    }
	}
    return found;
}

int
utah_brep_intersect(const SubsurfaceBBNode* sbv, const ON_BrepFace* face, const ON_Surface* surf, pt2d_t uv, ON_Ray& ray, HitList& hits)
{
    ON_3dVector N;
    bool hit = false;
    double t;
    ON_2dPoint ouv(uv[0], uv[1]);
    int found = BREP_INTERSECT_ROOT_DIVERGED;
    bool converged = false;

    utah_newton_solver( sbv, surf, ray, ouv, t, N, converged);
    /*
     * DDR.  The utah people are using this t_min which represents the
     * last point hit along the ray to ensure we are looking at points
     * futher down the ray.  I haven't implemented this I'm not sure
     * we need it
     *
     * if (converged && (t > 1.e-2) && (t < t_min) && (!utah_isTrimmed(ouv, face))) hit = true;
     *
     */
    //if (converged && (t > 1.e-2) && (!utah_isTrimmed(ouv, face))) hit = true;
	//if (converged && (t > 1.e-2) && (!((SubsurfaceBBNode*)sbv)->isTrimmed(ouv))) hit = true;
	
	if ( (sbv->m_u[0] < ouv[0]) && (sbv->m_u[1] > ouv[0]) &&
			(sbv->m_v[0] < ouv[1]) && (sbv->m_v[1] > ouv[1])) {
			
	if (converged && (t > 1.e-2)) {
		if  (!((SubsurfaceBBNode*)sbv)->isTrimmed(ouv)) {
			hit = true;
//#define KHITPLOT
#ifdef KHITPLOT
			double min[3],max[3];
			COLOR_PLOT(255, 200, 200);
			VSET(min,ouv[0]-0.01,ouv[1]-0.01,0.0);
			VSET(max,ouv[0]+0.01,ouv[1]+0.01,0.0);
			BB_PLOT(min,max);
		} else {
			double min[3],max[3];
			COLOR_PLOT(200, 255, 200);
			VSET(min,ouv[0]-0.01,ouv[1]-0.01,0.0);
			VSET(max,ouv[0]+0.01,ouv[1]+0.01,0.0);
			BB_PLOT(min,max);
		}
#else
	        }
#endif
	}
	}
	//    if (converged && (t > 1.e-2)) hit = true;

    uv[0] = ouv.x;
    uv[1] = ouv.y;

    if (hit) {
        ON_3dPoint _pt;
        ON_3dVector _norm(N);
        _pt = ray.m_origin + (ray.m_dir*t);
        if (face->m_bRev) _norm.Reverse();
        hit_count += 1;
        hits.push_back(brep_hit(*face,(const fastf_t*)ray.m_origin,(const fastf_t*)_pt,(const fastf_t*)_norm, uv));
        hits.back().sbv = sbv;
        found = BREP_INTERSECT_FOUND;
    }

    return found;
}


int
brep_intersect(const SubsurfaceBBNode* sbv, const ON_BrepFace* face, const ON_Surface* surf, pt2d_t uv, ON_Ray& ray, HitList& hits)
{
    int found = BREP_INTERSECT_ROOT_ITERATION_LIMIT;
    fastf_t Dlast = MAX_FASTF;
    int diverge_iter = 0;
    pt2d_t Rcurr;
    pt2d_t new_uv;
    ON_3dPoint pt;
    ON_3dVector su;
    ON_3dVector sv;
    plane_ray pr;
    brep_get_plane_ray(ray, pr);
    for (int i = 0; i < BREP_MAX_ITERATIONS; i++) {
	brep_r(surf, pr, uv, pt, su, sv, Rcurr);
	//fastf_t d = v2mag(Rcurr);
	//keith fastf_t d = DIST_PT_PT(pt, ray.m_origin);
	fastf_t d = v2mag(Rcurr);
	//keith
	//if (d < BREP_INTERSECTION_ROOT_EPSILON) {
	//keith if (NEAR_ZERO(d-Dlast, BREP_INTERSECTION_ROOT_EPSILON)) {
	if (d < BREP_INTERSECTION_ROOT_EPSILON) {
	    TRACE1("R:"<<ON_PRINT2(Rcurr));
	    found = BREP_INTERSECT_FOUND; break;
	} else if (d > Dlast) {
	    found = BREP_INTERSECT_ROOT_DIVERGED; //break;
	    diverge_iter++;
	    if (diverge_iter > 10)
		break;
	    //return brep_edge_check(found, sbv, face, surf, ray, hits);
	}
	brep_newton_iterate(surf, pr, Rcurr, su, sv, uv, new_uv);
	move(uv, new_uv);
	Dlast = d;
    }
    if ((found > 0) &&  (!((SubsurfaceBBNode*)sbv)->isTrimmed(uv))) {
	ON_3dPoint _pt;
	ON_3dVector _norm;
	surf->EvNormal(uv[0], uv[1],_pt,_norm);
	if (face->m_bRev) _norm.Reverse();
	hits.push_back(brep_hit(*face,(const fastf_t*)ray.m_origin,(const fastf_t*)_pt,(const fastf_t*)_norm, uv));
	hits.back().sbv = sbv;

 	if (!sbv->m_u.Includes(uv[0]) || !sbv->m_v.Includes(uv[1])) {
	    // 	    if (!sbv->m_u.Includes(uv[0]-BREP_SAME_POINT_TOLERANCE) ||
	    // 		!sbv->m_v.Includes(uv[1]-BREP_SAME_POINT_TOLERANCE)) {
	    hits.back().oob = true;
	    return BREP_INTERSECT_OOB;
	}


//	if (sbv->doTrimming() && brep_pt_trimmed(uv, *face)) {
//	    hits.back().trimmed = true;
//	    TRACE1("Should be TRIMMED!");
//	    // if the point was trimmed, see if it is close to the edge before removing it
//	    return brep_edge_check(BREP_INTERSECT_TRIMMED, sbv, face, surf, ray, hits);
//	    //return BREP_INTERSECT_TRIMMED;
//	}
    }

    return found;
}


#if 0
static void
opposite(const SubsurfaceBBNode* sbv, pt2d_t uv)
{
    if (uv[1] > sbv->m_v.Mid()) {
	// quadrant I or II
	uv[1] = sbv->m_v.Min();
	if (uv[0] > sbv->m_u.Mid())
	    // quad I
	    uv[0] = sbv->m_u.Min();
	else
	    // quad II
	    uv[0] = sbv->m_u.Max();
    } else {
	uv[1] = sbv->m_v.Max();
	if (uv[0] > sbv->m_u.Mid())
	    uv[0] = sbv->m_u.Min();
	else
	    uv[0] = sbv->m_u.Max();
    }
}
#endif


typedef std::pair<int, int> ip_t;
typedef std::list<ip_t> MissList;

static int
sign(double val)
{
    return (val >= 0.0) ? 1 : -1;
}


/**
 * R T _ B R E P _ S H O T
 *
 * Intersect a ray with a brep.  If an intersection occurs, a struct
 * seg will be acquired and filled in.
 *
 * Returns -
 * 	 0	MISS
 *	>0	HIT
 */
int
rt_brep_shot(struct soltab *stp, register struct xray *rp, struct application *ap, struct seg *seghead)
{
    //TRACE1("rt_brep_shot origin:" << ON_PRINT3(rp->r_pt) << " dir:" << ON_PRINT3(rp->r_dir));
    TRACE("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
    struct brep_specific* bs = (struct brep_specific*)stp->st_specific;

    // check the hierarchy to see if we have a hit at a leaf node
    BBNode::IsectList inters;
    ON_Ray r = toXRay(rp);
    bs->bvh->intersectsHierarchy(r, &inters);

    if (inters.size() == 0) return 0; // MISS
    TRACE1("bboxes: " << inters.size());
	
//#define KDEBUGMISS
	int boxcnt=0;
#ifdef KDEBUGMISS
	char buffer[80];
	icount++;
	if (icount > 1) return 0;
	sprintf(buffer,"Shot%d.pl",icount);
	plot_file((const char *)buffer);
	ON_3dPoint p = r.m_origin + (r.m_dir*20.0);
	point_t a,b;
	VMOVE(a,r.m_origin);
	VMOVE(b,p);
	COLOR_PLOT( 255, 255, 255 );
	LINE_PLOT(a,b);
	(void)fclose(plot_file());
#endif

    // find all the hits (XXX very inefficient right now!)
    HitList all_hits; // record all hits
    MissList misses; // XXX - get rid of this stuff (for debugging)
    int s = 0;
    hit_count = 0;
    for (BBNode::IsectList::iterator i = inters.begin(); i != inters.end(); i++) {
	const SubsurfaceBBNode* sbv = dynamic_cast<SubsurfaceBBNode*>((*i).m_node);
	
	boxcnt++;
#ifdef KDEBUGMISS
	sprintf(buffer,"N%d.pl",boxcnt);
	plot_file((const char *)buffer);
	plotleaf3d((SubsurfaceBBNode*)sbv);
	(void)fclose(plot_file());
	if (boxcnt == 1) { //plotting utah_brep_intersecthacks i==0) {
		const ON_BrepFace *face = sbv->m_face;
		sprintf(buffer,"Face%d_N%d.pl",face->m_face_index,boxcnt);
		plot_file((const char *)buffer);
		plottrim((ON_BrepFace&)*sbv->m_face);
		(void)fclose(plot_file());
		sprintf(buffer,"Tree%d_N%d.pl",face->m_face_index,boxcnt);
		plot_file((const char *)buffer);
		SurfaceTree* st = (SurfaceTree*)face->m_face_user.p;
		plotsurfaceleafs(st);
		(void)fclose(plot_file());
	}
#endif

	const ON_BrepFace* f = sbv->m_face;
	const ON_Surface* surf = f->SurfaceOf();
	pt2d_t uv = {sbv->m_u.Mid(), sbv->m_v.Mid()};
	TRACE1("surface: " << s);
	int status = utah_brep_intersect_test(sbv, f, surf, uv, r, all_hits);
	if (status == BREP_INTERSECT_FOUND) {
	    TRACE("INTERSECTION: " << PT(all_hits.back().point) << all_hits.back().trimmed << ", " << all_hits.back().closeToEdge << ", " << all_hits.back().oob);
	} else {
	    static int reasons = 0;
	    if (reasons < 100) {
		TRACE2(BREP_INTERSECT_REASON((brep_intersect_reason_t)status));
		reasons++;
	    } else {
		static int quelled = 0;
		if (!quelled) {
		    TRACE2("Too many reasons.  Suppressing further output." << std::endl);
		    quelled = 1;
		}
	    }
	    misses.push_back(ip_t(all_hits.size()-1, status));
	}
	s++;
    }

#ifdef KDEBUGMISS
	//(void)fclose(plot_file());
#endif
    HitList hits = all_hits;

    // sort the hits
    hits.sort();

    TRACE("---");
    int num = 0;
    for (HitList::iterator i = hits.begin(); i != hits.end(); ++i) {
	if ((i->trimmed && !i->closeToEdge) || i->oob || NEAR_ZERO(VDOT(i->normal, rp->r_dir), RT_DOT_TOL)) {
	    // remove what we were removing earlier
	    if (i->oob) {
		TRACE("\toob u: " << i->uv[0] << ", " << IVAL(i->sbv->m_u));
		TRACE("\toob v: " << i->uv[1] << ", " << IVAL(i->sbv->m_v));
	    }
	    i = hits.erase(i);

	    if (i != hits.begin())
		--i;

	    continue;
	}
	TRACE("hit " << num << ": " << PT(i->point) << " [" << VDOT(i->normal, rp->r_dir) << "]");
	++num;
    }

    if (hits.size() > 0) {
	// we should have "valid" points now, remove duplicates or grazes
	HitList::iterator last = hits.begin();
	HitList::iterator i = hits.begin();
	++i;
	while (i != hits.end()) {
	    if ((*i) == (*last)) {
		double lastDot = VDOT(last->normal, rp->r_dir);
		double iDot = VDOT(i->normal, rp->r_dir);

		if (sign(lastDot) != sign(iDot)) {
		    // delete them both
		    i = hits.erase(last);
		    i = hits.erase(i);
		    last = i;

		    if (i != hits.end())
			++i;
		} else {
		    // just delete the second
		    i = hits.erase(i);
		}
	    } else {
		last = i;
		++i;
	    }
	}
    }
/*
    if (hits.size() > 1 && (hits.size() % 2) != 0) {
        HitList::iterator i = hits.end();
        --i;
        hits.erase(i);
    }
    if (hits.size() > 1 && (hits.size() % 2) != 0) {
        HitList::iterator i = hits.begin();
        hits.erase(i);
    }
*/

    // remove "duplicate" points
    //     HitList::iterator new_end = unique(hits.begin(), hits.end());
    //     hits.erase(new_end, hits.end());

    if (hits.size() > 1 && (hits.size() % 2) != 0) {
        cerr << "**** ERROR odd number of hits: " << hits.size() << "\n";
        bu_log("xyz %f %f %f \n", rp->r_pt[0], rp->r_pt[1], rp->r_pt[2]);
	bu_log("dir %f %f %f \n", rp->r_dir[0], rp->r_dir[1], rp->r_dir[2]);
	
        point_t last_point;
        int hitCount = 0;
        for (HitList::iterator i = hits.begin(); i != hits.end(); ++i) {
            if (hitCount == 0) {
                TRACE2("point: " << i->point[0] << "," << i->point[1] << "," << i->point[2] << " dist_to_ray: " << DIST_PT_PT(i->point, rp->r_pt));
            } else {
                TRACE2("point: " << i->point[0] << "," << i->point[1] << "," << i->point[2] << " dist_to_ray: " << DIST_PT_PT(i->point, rp->r_pt) << " dist_to_last_point: " << DIST_PT_PT(i->point, last_point));
            }
            VMOVE(last_point, i->point);
            hitCount += 1;
        }
#if PLOTTING
	pcount++;
	if (pcount > -1) {
	    point_t ray;
	    point_t vscaled;
	    VSCALE(vscaled, rp->r_dir, 100);
	    VADD2(ray, rp->r_pt, vscaled);
	    COLOR_PLOT(200, 200, 200);
	    LINE_PLOT(rp->r_pt, ray);
	}
#endif

	num = 0;
	MissList::iterator m = misses.begin();
	for (HitList::iterator i = all_hits.begin(); i != all_hits.end(); ++i) {

#if PLOTTING
	    if (pcount > -1) {
		// set the color of point and normal
		if (i->trimmed && i->closeToEdge) {
		    COLOR_PLOT(0, 0, 255); // blue is trimmed but close to edge
		} else if (i->trimmed) {
		    COLOR_PLOT(255, 255, 0); // yellow trimmed
		} else if (i->oob) {
		    COLOR_PLOT(255, 0, 0); // red is oob
		} else if (NEAR_ZERO(VDOT(i->normal, rp->r_dir), RT_DOT_TOL)) {
		    COLOR_PLOT(0, 255, 255); // purple is grazing
		} else {
		    COLOR_PLOT(0, 255, 0); // green is regular surface
		}

		// draw normal
		point_t v;
		VADD2(v, i->point, i->normal);
		LINE_PLOT(i->point, v);

		// draw intersection
		PT_PLOT(i->point);

		// draw bounding box
		BB_PLOT(i->sbv->m_node.m_min, i->sbv->m_node.m_max);
		fflush(plot_file());
	    }
#endif

	    // 	    if ((num == 0 && dot > 0) || sign(dot) == lastSign) {
	    // remove hits with "bad" normals
	    // 		i = hits.erase(i);
	    // 		--i;
	    // 		TRACE("removed a hit!");
	    // 		continue;
	    // 	    } else {
	    // 		lastSign = sign(dot);
	    // 	    }

	    TRACE("hit " << num << ": " << ON_PRINT3(i->point) << " [" << dot << "]");
	    while ((m != misses.end()) && (m->first == num)) {
		static int reasons = 0;
		if (reasons < 100) {
		    reasons++;
		    TRACE("miss " << num << ": " << BREP_INTERSECT_REASON(m->second));
		} else {
		    static int quelled = 0;
		    if (!quelled) {
			TRACE("Too many reasons.  Suppressing further output." << std::endl);
			quelled = 1;
		    }
		}
		++m;
	    }
	    num++;
	}
	while (m != misses.end()) {
	    static int reasons = 0;
	    if (reasons < 100) {
		reasons++;
		TRACE("miss " << BREP_INTERSECT_REASON(m->second));
	    } else {
		static int quelled = 0;
		if (!quelled) {
		    TRACE("Too many reasons.  Suppressing further output." << std::endl);
		    quelled = 1;
		}
	    }
	    ++m;
	}
    }

    bool hit = false;
    if (hits.size() > 0) {
//#define KODDHIT
#ifdef KODDHIT //ugly debugging hack to raytrace single surface and not worry about odd hits
	static fastf_t diststep = 0.0;
	if (hits.size() > 0 ) { 
#else
	if (hits.size() % 2 == 0) {
#endif
	    // take each pair as a segment
	    for (HitList::iterator i = hits.begin(); i != hits.end(); ++i) {
		brep_hit& in = *i;
#ifndef KODDHIT  //ugly debugging hack to raytrace single surface and not worry about odd hits
		i++;
#endif
		brep_hit& out = *i;

		register struct seg* segp;
		RT_GET_SEG(segp, ap->a_resource);
		segp->seg_stp = stp;

		VMOVE(segp->seg_in.hit_point, in.point);
		VMOVE(segp->seg_in.hit_normal, in.normal);
#ifdef KODDHIT //ugly debugging hack to raytrace single surface and not worry about odd hits
		segp->seg_in.hit_dist = diststep + 1.0;
#else
		segp->seg_in.hit_dist = DIST_PT_PT(rp->r_pt, in.point);
#endif
		segp->seg_in.hit_surfno = in.face.m_face_index;
		VSET(segp->seg_in.hit_vpriv, in.uv[0], in.uv[1], 0.0);

		VMOVE(segp->seg_out.hit_point, out.point);
		VMOVE(segp->seg_out.hit_normal, out.normal);
		segp->seg_out.hit_dist = DIST_PT_PT(rp->r_pt, out.point);
		segp->seg_out.hit_surfno = out.face.m_face_index;
		VSET(segp->seg_out.hit_vpriv, out.uv[0], out.uv[1], 0.0);

		BU_LIST_INSERT(&(seghead->l), &(segp->l));
	    }
	    hit = true;
	} else {
	    //TRACE2("screen xy: " << ap->a_x << "," << ap->a_y);
	}
    }

    return (hit) ? hits.size() : 0; // MISS
}


/**
 * R T _ B R E P _ N O R M
 *
 * Given ONE ray distance, return the normal and entry/exit point.
 */
void
rt_brep_norm(register struct hit *hitp, struct soltab *stp, register struct xray *rp)
{

}


/**
 * R T _ B R E P _ C U R V E
 *
 * Return the curvature of the nurb.
 */
void
rt_brep_curve(register struct curvature *cvp, register struct hit *hitp, struct soltab *stp)
{
    /* XXX todo */
}


/**
 * R T _ B R E P _ C L A S S
 *
 * Don't know what this is supposed to do...
 *
 * Looking at g_arb.c, seems the actual signature is:
 *
 * class(const struct soltab* stp,
 *       const fastf_t* min,
 *       const fastf_t* max,
 *       const struct bn_tol* tol)
 */
int
rt_brep_class()
{
    return RT_CLASSIFY_UNIMPLEMENTED;
}


/**
 * R T _ B R E P _ U V
 *
 * For a hit on the surface of an nurb, return the (u, v) coordinates
 * of the hit point, 0 <= u, v <= 1.
 * u = azimuth
 * v = elevation
 */
void
rt_brep_uv(struct application *ap, struct soltab *stp, register struct hit *hitp, register struct uvcoord *uvp)
{
    uvp->uv_u = hitp->hit_vpriv[0];
    uvp->uv_v = hitp->hit_vpriv[1];
}


/**
 * R T _ B R E P _ F R E E
 */
void
rt_brep_free(register struct soltab *stp)
{
    TRACE1("rt_brep_free");
    struct brep_specific* bs = (struct brep_specific*)stp->st_specific;
    brep_specific_delete(bs);
}

//
//  Given surface tree bounding box information, plot the bounding box
//  as a wireframe in mged.
//
void
plot_bbnode(BBNode* node, struct bu_list* vhead, int depth, int start, int limit) {
    ON_3dPoint min = node->m_node.m_min;
    ON_3dPoint max = node->m_node.m_max;
    point_t verts[] = {{min[0], min[1], min[2]},
		       {min[0], max[1], min[2]},
		       {min[0], max[1], max[2]},
		       {min[0], min[1], max[2]},
		       {max[0], min[1], min[2]},
		       {max[0], max[1], min[2]},
		       {max[0], max[1], max[2]},
		       {max[0], min[1], max[2]}};

//    if (node->isLeaf()) {
    if (depth >= start && depth<=limit) {
	for (int i = 0; i <= 4; i++) {
	    RT_ADD_VLIST(vhead, verts[i%4], (i == 0) ? BN_VLIST_LINE_MOVE : BN_VLIST_LINE_DRAW);
	}
	for (int i = 0; i <= 4; i++) {
	    RT_ADD_VLIST(vhead, verts[(i%4)+4], (i == 0) ? BN_VLIST_LINE_MOVE : BN_VLIST_LINE_DRAW);
	}
	for (int i = 0; i < 4; i++) {
	    RT_ADD_VLIST(vhead, verts[i], BN_VLIST_LINE_MOVE);
	    RT_ADD_VLIST(vhead, verts[i+4], BN_VLIST_LINE_DRAW);
	}

    }

    for (size_t i = 0; i < node->m_children.size(); i++) {
	if (i < 1)
	plot_bbnode(node->m_children[i], vhead, depth+1, start, limit);
    }
}

double
find_next_point(const ON_Curve* crv, double startdomval, double increment, double tolerance, int stepcount) {
    double inc = increment;
    if (startdomval + increment > 1.0) inc = 1.0 - startdomval;
    ON_Interval dom = crv->Domain();
    ON_3dPoint prev_pt = crv->PointAt(dom.ParameterAt(startdomval));
    ON_3dPoint next_pt = crv->PointAt(dom.ParameterAt(startdomval+inc));
    if (prev_pt.DistanceTo(next_pt) > tolerance) {
	stepcount++;
	inc = inc / 2;
	return find_next_point(crv, startdomval, inc, tolerance, stepcount);
    } else {
	if (stepcount > 5) return 0.0;
	return startdomval + inc;
    }
}

double
find_next_trimming_point(const ON_Curve* crv, const ON_Surface* s, double startdomval, double increment, double tolerance, int stepcount) {
    double inc = increment;
    if (startdomval + increment > 1.0) inc = 1.0 - startdomval;
    ON_Interval dom = crv->Domain();
    ON_3dPoint prev_pt = crv->PointAt(dom.ParameterAt(startdomval));
    ON_3dPoint next_pt = crv->PointAt(dom.ParameterAt(startdomval+inc));
    ON_3dPoint prev_3d_pt, next_3d_pt;
    s->EvPoint(prev_pt[0],prev_pt[1],prev_3d_pt,0,0);
    s->EvPoint(next_pt[0],next_pt[1],next_3d_pt,0,0);
    if (prev_3d_pt.DistanceTo(next_3d_pt) > tolerance) {
	stepcount++;
	inc = inc / 2;
	return find_next_trimming_point(crv, s, startdomval, inc, tolerance, stepcount);
    } else {
	if (stepcount > 5) return 0.0;
	return startdomval + inc;
    }
}
    
/**
 * R T _ B R E P _ P L O T
 *
 * There are several ways to visualize NURBS surfaces, depending on
 * the purpose.  For "normal" wireframe viewing, the ideal approach
 * is to do a tesselation of the NURBS surface and show that wireframe.
 * The quicker and simpler approach is to visualize the edges, although
 * that can sometimes generate less than ideal/useful results (for example,
 * a revolved edge that forms a sphere will have a wireframe consisting of a
 * 2D arc in MGED when only edges are used.)  A third approach is to walk
 * the uv space for each surface, find 3space points at uv intervals, and
 * draw lines between the results - this is slightly more comprehensive
 * when it comes to showing where surfaces are in 3space but looks blocky
 * and crude.  For now, edge-only wireframes are the default.
 * 
 */
int
rt_brep_plot(struct bu_list *vhead, struct rt_db_internal *ip, const struct rt_tess_tol *ttol, const struct bn_tol *tol)
{
    TRACE1("rt_brep_plot");
    struct rt_brep_internal* bi;
    int i, j, k;

    RT_CK_DB_INTERNAL(ip);
    bi = (struct rt_brep_internal*)ip->idb_ptr;
    RT_BREP_CK_MAGIC(bi);

    /* XXX below does NOT work for non-trivial faces, in addition to
     * the fact that openNURBS does NOT support meshes!
     *
     * XXX currently not handling the tolerances.
     *
     *     ON_MeshParameters mp;
     *     mp.JaggedAndFasterMeshParameters();
     *
     *     ON_SimpleArray<ON_Mesh*> mesh_list;
     *     bi->brep->CreateMesh(mp, mesh_list);
     *
     *     point_t pt1, pt2;
     *     ON_SimpleArray<ON_2dex> edges;
     *     for (int i = 0; i < mesh_list.Count(); i++) {
     *  	const ON_Mesh* mesh = mesh_list[i];
     *  	mesh->GetMeshEdges(edges);
     *  	for (int j = 0; j < edges.Count(); j++) {
     *  	    ON_MeshVertexRef v1 = mesh->VertexRef(edges[j].i);
     *  	    ON_MeshVertexRef v2 = mesh->VertexRef(edges[j].j);
     *  	    VSET(pt1, v1.Point().x, v1.Point().y, v1.Point().z);
     *  	    VSET(pt2, v2.Point().x, v2.Point().y, v2.Point().z);
     *  	    RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_MOVE);
     *  	    RT_ADD_VLIST(vhead, pt2, BN_VLIST_LINE_DRAW);
     *  	}
     *  	edges.Empty();
     *     }
     *
     * So we'll do it by hand by grabbing each topological edge from
     * the brep and rendering it that way...
     */
    ON_Brep* brep = bi->brep;

    point_t pt1, pt2;

    for (i = 0; i < bi->brep->m_E.Count(); i++) {
	ON_BrepEdge& e = brep->m_E[i];
	const ON_Curve* crv = e.EdgeCurveOf();

	if (crv->IsLinear()) {
	    ON_BrepVertex& v1 = brep->m_V[e.m_vi[0]];
	    ON_BrepVertex& v2 = brep->m_V[e.m_vi[1]];
	    VMOVE(pt1, v1.Point());
	    VMOVE(pt2, v2.Point());
	    RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_MOVE);
	    RT_ADD_VLIST(vhead, pt2, BN_VLIST_LINE_DRAW);
	} else {
	    ON_Interval dom = crv->Domain();

	    double domainval = 0.0;
	    double olddomainval = 1.0;
	    int crudestep = 0;
	    // Insert first point.
	    ON_3dPoint p = crv->PointAt(dom.ParameterAt(domainval));
	    VMOVE(pt1, p);
	    RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_MOVE);
	    
	    // Dynamic sampling approach - start with an initial guess for the
	    // next point of one tenth of the domain length further down
	    // the domain from the previous value.  Set a maximum physical
	    // distance between points of 100 times the model tolerance.
	    // Reduce the increment until the tolerance is satisfied, then
	    // add the point and use it as the starting point for the next
	    // calculation until the whole domain is finished.
	    // Perhaps it would be more ideal to base the tolerance on some
	    // fraction of the curve bounding box dimensions?
	    while (domainval < 1.0 && crudestep <= 100) {
		olddomainval = domainval;
		if (crudestep == 0) domainval = find_next_point(crv, domainval, 0.1, tol->dist*100, 0);
		if (crudestep >= 1 || domainval == 0.0) {
		    crudestep++;
		    domainval =  olddomainval + (1.0 - olddomainval)/100*crudestep;
		}
		ON_3dPoint p = crv->PointAt(dom.ParameterAt(domainval));
		VMOVE(pt1, p);
		RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_DRAW);
	    }
	}
    }

    
/*
 * DEBUGGING WIREFRAMES
 */
    
//   Routine to draw the bounding boxes in the surface
//   tree.
//
//         for (int i = 0; i < brep->m_F.Count(); i++) {
//           ON_BrepFace& f = brep->m_F[i];
//           SurfaceTree st(&f);
//           plot_bbnode(st.getRootNode(), vhead, 0, 1, 8);
//         }


    /* Routine to iterate over the surfaces in the BREP and plot lines corresponding
     * to their projections into 3-space.  Very crude walk method - doesn't properly
     * handle drawing in the case of trims - but it illustrates how to go straight
     * from uv parameter space to real space coordinates.
     * Needs to become proper tesselation routine.
     */

/*    for (i = 0; i < bi->brep->m_F.Count(); i++) {
	ON_BrepFace *f = &(bi->brep->m_F[i]);
	const ON_Surface *s = bi->brep->m_F[i].SurfaceOf();
	int foundfirst = 0;
	for (j = 0; j <= 20; j++) {
	    for (k = 0; k <= 20; k++) {
		ON_2dPoint uv;
		ON_3dPoint plotpt;
		uv.x = s->Domain(0).ParameterAt((double)j/20);
		uv.y = s->Domain(1).ParameterAt((double)k/20);
		    s->EvPoint(uv.x,uv.y,plotpt,0,0);
		    VMOVE(pt1, plotpt);
		    if (j == 0 || foundfirst == 0) {
			RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_MOVE);
		        foundfirst = 1;
		    } else {
    			RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_DRAW);
    		    }
	    }
	}
    }*/


    /* Routine to iterate over the surfaces in the BREP and plot lines corresponding
     * to the trimming curve positions in 3-space.  Normally, this will correspond pretty
     * well to edges, but will show some cases where two surfaces are intended to join
     * without an edge
     */
/*
    for (i = 0; i < bi->brep->m_F.Count(); i++) {
	ON_BrepFace *f = &(bi->brep->m_F[i]);
	const ON_Surface *s = bi->brep->m_F[i].SurfaceOf();
        for (j = 0; j < f->LoopCount(); j++) {
	    ON_BrepLoop* loop = f->Loop(j);
	    int foundfirst = 0;
    		for (int k = 0; k < loop->m_ti.Count(); k++) {
    		    ON_BrepTrim& trim = f->Brep()->m_T[loop->m_ti[k]];
    		    const ON_Curve* trimCurve = trim.TrimCurveOf();
		    ON_Interval dom = trimCurve->Domain();
		    double domainval = 0.0;
		    double olddomainval = 1.0;
		    int crudestep = 0;
		    ON_3dPoint trimpoint2d = trimCurve->PointAt(dom.ParameterAt(domainval));
		    ON_3dPoint trimpoint3d;
		    s->EvPoint(trimpoint2d[0],trimpoint2d[1],trimpoint3d,0,0);
		    VMOVE(pt1, trimpoint3d);
		    RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_MOVE);
		    while (domainval < 1.0 && crudestep <= 100) {
			olddomainval = domainval;
			if (crudestep == 0) domainval = find_next_trimming_point(trimCurve, s, domainval, 0.1, tol->dist*100, 0);
			if (crudestep >= 1 || domainval == 0.0) {
			    crudestep++;
			    domainval =  olddomainval + (1.0 - olddomainval)/100*crudestep;
			}
			trimpoint2d = trimCurve->PointAt(dom.ParameterAt(domainval));
			s->EvPoint(trimpoint2d[0],trimpoint2d[1],trimpoint3d,0,0);
			VMOVE(pt1, trimpoint3d);
			RT_ADD_VLIST(vhead, pt1, BN_VLIST_LINE_DRAW);
		    }
		}
	}
    }
*/		


    return 0;
}


/**
 * R T _ B R E P _ T E S S
 */
int
rt_brep_tess(struct nmgregion **r, struct model *m, struct rt_db_internal *ip, const struct rt_tess_tol *ttol, const struct bn_tol *tol)
{
    return -1;
}


/**
 * XXX In order to facilitate exporting the ON_Brep object without a
 * whole lot of effort, we're going to (for now) extend the
 * ON_BinaryArchive to support an "in-memory" representation of a
 * binary archive. Currently, the openNURBS library only supports
 * file-based archiving operations. This implies the
 */
class RT_MemoryArchive : public ON_BinaryArchive
{
public:
    RT_MemoryArchive();
    RT_MemoryArchive(genptr_t memory, size_t len);
    virtual ~RT_MemoryArchive();

    // ON_BinaryArchive overrides
    size_t CurrentPosition() const;
    bool SeekFromCurrentPosition(int);
    bool SeekFromStart(size_t);
    bool AtEnd() const;

    size_t Size() const;
    /**
     * Generate a byte-array copy of this memory archive.  Allocates
     * memory using bu_malloc, so must be freed with bu_free
     */
    genptr_t CreateCopy() const;

protected:
    size_t Read(size_t, void*);
    size_t Write(size_t, const void*);
    bool Flush();

private:
    size_t pos;
    std::vector<char> m_buffer;
};

RT_MemoryArchive::RT_MemoryArchive()
    : ON_BinaryArchive(ON::write3dm), pos(0)
{
}

RT_MemoryArchive::RT_MemoryArchive(genptr_t memory, size_t len)
    : ON_BinaryArchive(ON::read3dm), pos(0)
{
    m_buffer.reserve(len);
    for (size_t i = 0; i < len; i++) {
	m_buffer.push_back(((char*)memory)[i]);
    }
}

RT_MemoryArchive::~RT_MemoryArchive()
{
}

size_t
RT_MemoryArchive::CurrentPosition() const
{
    return pos;
}

bool
RT_MemoryArchive::SeekFromCurrentPosition(int seek_to)
{
    if (pos + seek_to > m_buffer.size()) return false;
    pos += seek_to;
    return true;
}

bool
RT_MemoryArchive::SeekFromStart(size_t seek_to)
{
    if (seek_to > m_buffer.size()) return false;
    pos = seek_to;
    return true;
}

bool
RT_MemoryArchive::AtEnd() const
{
    return pos >= m_buffer.size();
}

size_t
RT_MemoryArchive::Size() const
{
    return m_buffer.size();
}

genptr_t
RT_MemoryArchive::CreateCopy() const
{
    genptr_t memory = (genptr_t)bu_malloc(m_buffer.size()*sizeof(char),"rt_memoryarchive createcopy");
    const int size = m_buffer.size();
    for (int i = 0; i < size; i++) {
	((char*)memory)[i] = m_buffer[i];
    }
    return memory;
}

size_t
RT_MemoryArchive::Read(size_t amount, void* buf)
{
    const int read_amount = (pos + amount > m_buffer.size()) ? m_buffer.size()-pos : amount;
    const size_t start = pos;
    for (; pos < (start+read_amount); pos++) {
	((char*)buf)[pos-start] = m_buffer[pos];
    }
    return read_amount;
}

size_t
RT_MemoryArchive::Write(const size_t amount, const void* buf)
{
    // the write can come in at any position!
    const int start = pos;
    // resize if needed to support new data
    if (m_buffer.size() < (start+amount)) {
	m_buffer.resize(start+amount);
    }
    for (; pos < (start+amount); pos++) {
	m_buffer[pos] = ((char*)buf)[pos-start];
    }
    return amount;
}

bool
RT_MemoryArchive::Flush()
{
    return true;
}


/**
 * R T _ B R E P _ E X P O R T 5
 */
int
rt_brep_export5(struct bu_external *ep, const struct rt_db_internal *ip, double local2mm, const struct db_i *dbip)
{
    TRACE1("rt_brep_export5");
    struct rt_brep_internal* bi;

    RT_CK_DB_INTERNAL(ip);
    if (ip->idb_type != ID_BREP) return -1;
    bi = (struct rt_brep_internal*)ip->idb_ptr;
    RT_BREP_CK_MAGIC(bi);

    BU_INIT_EXTERNAL(ep);

    RT_MemoryArchive archive;
    /* XXX what to do about the version */
    ONX_Model model;

    {
	ON_Layer default_layer;
	default_layer.SetLayerIndex(0);
	default_layer.SetLayerName("Default");
	model.m_layer_table.Reserve(1);
	model.m_layer_table.Append(default_layer);
    }

    ONX_Model_Object& mo = model.m_object_table.AppendNew();
    mo.m_object = bi->brep;
    mo.m_attributes.m_layer_index = 0;
    mo.m_attributes.m_name = "brep";
    mo.m_attributes.m_uuid = ON_opennurbs4_id;

    model.m_properties.m_RevisionHistory.NewRevision();
    model.m_properties.m_Application.m_application_name = "BRL-CAD B-Rep primitive";

    model.Polish();
    ON_TextLog err(stderr);
    bool ok = model.Write(archive, 4, "export5", &err);
    if (ok) {
	ep->ext_nbytes = archive.Size();
	ep->ext_buf = archive.CreateCopy();
	return 0;
    } else {
	return -1;
    }
}


/**
 * R T _ B R E P _ I M P O R T 5
 */
int
rt_brep_import5(struct rt_db_internal *ip, const struct bu_external *ep, register const fastf_t *mat, const struct db_i *dbip)
{
    ON::Begin();
    TRACE1("rt_brep_import5");
    struct rt_brep_internal* bi;
    BU_CK_EXTERNAL(ep);
    RT_CK_DB_INTERNAL(ip);
    ip->idb_major_type = DB5_MAJORTYPE_BRLCAD;
    ip->idb_type = ID_BREP;
    ip->idb_meth = &rt_functab[ID_BREP];
    ip->idb_ptr = bu_malloc(sizeof(struct rt_brep_internal), "rt_brep_internal");

    bi = (struct rt_brep_internal*)ip->idb_ptr;
    bi->magic = RT_BREP_INTERNAL_MAGIC;

    RT_MemoryArchive archive(ep->ext_buf, ep->ext_nbytes);
    ONX_Model model;
    ON_TextLog dump(stderr);
    //archive.Dump3dmChunk(dump);
    model.Read(archive, &dump);

    if (model.IsValid(&dump)) {
	ONX_Model_Object mo = model.m_object_table[0];
	// XXX does openNURBS force us to copy? it seems the answer is
	// YES due to the const-ness
	bi->brep = new ON_Brep(*ON_Brep::Cast(mo.m_object));
	return 0;
    } else {
	return -1;
    }
}


/**
 * R T _ B R E P _ I F R E E
 */
void
rt_brep_ifree(struct rt_db_internal *ip, struct resource *resp)
{
    struct rt_brep_internal* bi;
    if (!resp) resp = &rt_uniresource;
    RT_CK_DB_INTERNAL(ip);

    TRACE1("rt_brep_ifree");

    bi = (struct rt_brep_internal*)ip->idb_ptr;
    RT_BREP_CK_MAGIC(bi);
    if (bi->brep != NULL)
	delete bi->brep;
    bu_free(bi, "rt_brep_internal free");
    ip->idb_ptr = GENPTR_NULL;
}


/**
 * R T _ B R E P _ D E S C R I B E
 */
int
rt_brep_describe(struct bu_vls *str, const struct rt_db_internal *ip, int verbose, double mm2local)
{
    BU_CK_VLS(str);
    RT_CK_DB_INTERNAL(ip);

    ON_wString wonstr;
    ON_TextLog log(wonstr);

    struct rt_brep_internal* bi;
    bi = (struct rt_brep_internal*)ip->idb_ptr;
    RT_BREP_CK_MAGIC(bi);
    if (bi->brep != NULL)
	bi->brep->Dump(log);

    ON_String onstr = ON_String(wonstr);
    bu_vls_strcat(str, "Boundary Representation (BREP) object\n");

    const char *description = onstr.Array();
    // skip the first "ON_Brep:" line
    while (description && description[0] && description[0] != '\n') {
	description++;
    }
    if (description && description[0] && description[0] == '\n') {
	description++;
    }
    bu_vls_strcat(str, description);

    return 0;
}


/**
 * R T _ B R E P _ T C L G E T
 */
int
rt_brep_tclget(Tcl_Interp *interp, const struct rt_db_internal *intern, const char *attr)
{
    return 0;
}


/**
 * R T _ B R E P _ T C L A D J U S T
 */
int
rt_brep_tcladjust(Tcl_Interp *interp, struct rt_db_internal *intern, int argc, char **argv)
{
    return 0;
}


/**
 * R T _ B R E P _ P A R A M S
 */
int
rt_brep_params(struct pc_pc_set *, const struct rt_db_internal *ip)
{
    return 0;
}

/** @} */

/*
 * Local Variables:
 * mode: C++
 * tab-width: 8
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * c-file-style: "stroustrup"
 * End:
 * ex: shiftwidth=4 tabstop=8
 */
