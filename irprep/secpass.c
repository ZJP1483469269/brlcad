/*  File:  secpass.c  */
/*  S.Coates - 13 May 1991  */
/*  This version ONLY shoots down x-axis.  */

/*	CHANGES		*/
/*	11 December 1990 - 'Dimension' arrays using malloc.  */
/*	18 December 1990 - Incorperates subroutines rotate and radians.  */
/*	19 February 1991 - No defaults for material properties, thermal  */
/*			   conductivity is zero if one is zero.  */
/*	25 February 1991 - Print triangular conductivity file (still  */
/*			   computes entire matrix).  */
/*	14 March 1991    - Creates a generic file if needed.  */
/*	13 May 1991      - Remove some write statements.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*  The following are needed when using rt_shootray.  */

#include "machine.h"
#include "vmath.h"
#include "raytrace.h"

#define ADJTOL 1.e-1	/*  Tolerance for adjacent regions.  */
#define ZEROTOL 1.e-20	/*  Tolerance for dividing by zero.  */
#define MINCAL 10	/*  Minimum number of calculations for length.  */
#define PI 3.14159265358979323	/*  Pi.  */
#define ALPHA 25.	/*  Rotation about z-axis.  */
#define BETA 50.	/*  Rotation about y-axis.  */
#define GAMMA 35.	/*  Rotation about x-axis.  */

struct application ap;	/*  Structure passed between functions.  */
extern int hit();	/*  User defined hit function.  */
extern int miss();	/*  User defined miss function.  */
extern int ovrlap();	/*  User defined overlap function.  */
extern void rotate();	/*  Subroutine to rotate a point.  */
extern double radians();/*  Subroutine to put an angle into radians.  */

/*  Define structure.  */
/*  Make arrays into pointers such that malloc may be used  */
/*  later.  */
struct table
{
	double centroid[3];	/*  centroid of region  */
	double *shrarea;	/*  shared surface area between adjacent  */
				/*  regions  */
	int mat;		/*  material id  */
	double *avglen;		/*  average length between centroid &  */
				/*  shared surface area  */
	double *rmslen;		/*  root mean squared length between  */
				/*  centroid & shared surface area  */
	double *minlen;		/*  minimum length between centroid &  */
				/*  shared surface area  */
	double *maxlen;		/*  maximum length between centroid &  */
				/*  shared surface area  */
	double *numcal;		/*  number of length calculations  */
	double *rkavg;		/*  rk value using average length  */
	double *rkrms;		/*  rk value using root mean squared length  */
	double *rkmin;		/*  rk value using minimum length  */
	double *rkmax;		/*  rk value using maximum length  */
};

struct table *cond;		/*  name of table structure  */

int iprev;			/*  previous region hit  */
int icur;			/*  current region hit  */
double enterpt[3];		/*  point where ray enters  */
double leavept[3];		/*  point where ray leaves  */

main(argc,argv)

int argc;
char **argv[];

{	/*  START # 1  */

   static struct rt_i *rtip;	/*  *rtip pointer to structure of  */
				/*  type rt_i  */
   char idbuf[132];	/*  first ID record info, used in  */
			/*  rt_dirbuild  */
   int index;		/*  index for rt_dirbuild & rt_gettree  */

   FILE *fp,*fopen();	/*  used in opening file for second pass  */
   char spfile[16];	/*  second pass file name  */
   FILE *fp1,*fopen();	/*  conductivity file  */
   char confile[16];	/*  conductivity file  */
   FILE *fp2,*fopen();	/*  conductivity table file  */
   char tblfile[16];	/*  conductivity table file  */

   int i,j;		/*  integers used in loops  */
   int numreg;		/*  number of regions  */
   int nmged;		/*  number of regions in mged file  */
   double gridspace;	/*  spacing between fired rays  */
   int iwrite;		/*  0 => write to standard out, 1 => write  */
			/*  to file  */
   int typeout;		/*  Type of file to be written, 0 => PRISM file,  */
			/*  1 => generic file.  */
   FILE *fp6,*fopen();	/*  Used in writine generic file.  */
   char genfile[16];	/*  Generic file name.  */
   FILE *fp3,*fopen();	/*  used for writing output to file  */
   char filename[16];	/*  output file name  */

   FILE *fp5,*fopen();	/*  material file  */
   char filemat[16];	/*  material file  */
   char line[150];	/*  used for reading a line from a file  */
   double k[41];	/*  thermal conductivity  */
   int ia;		/*  used to find material type for region i  */
   int ja;		/*  used to find material type for region j  */
   int itype;		/*  type of length measurement to use for  */
			/*  rk calculations  */
   double rki,rkj;	/*  used in finding rk  */
   double ki,kj;	/*  thermal conductivity of region  */
   double leni,lenj;	/*  lengths used in finding rk  */
   double areai,areaj;	/*  areas used in finding rk  */
   double a1;		/*  area used in writing conductivity table  */
   double l1,l2,l3,l4;	/*  lengths used in writing conductivity table  */
   int ians;		/*  used for answering questions  */
   FILE *fp4,*fopen();	/*  error file  */
   char fileerr[16];	/*  error file  */

   double angle[3];	/*  Angles of rotation.  angle[0]-rotation about  */
			/*  x-axis, angle[1]-rotation about y-axis, &  */
			/*  angle[2]-rotation about z-axis.  */
   double strtpt[3];	/*  Starting point of fired ray.  */
   double strtdir[3];	/*  Starting direction of fired ray.  */
   double r[3],t[3];	/*  Used in computing rotations.  */
   double center[3];	/*  Center point of bounding rpp.  */
   double diagonal;	/*  Length of diagonal of bounding rpp.  */
   double xmin,xmax;	/*  Maximum & minimum x of grid.  */
   double ymin,ymax;	/*  Maximum & minimum y of grid.  */
   double zmin,zmax;	/*  Maximum & minimum z of grid.  */
   int nadjreg;		/*  Number of adjacent regions.  */

   /*  Check to see if arguments implimented correctly.  */
   if(argv[1] == NULL || argv[2] == NULL)
   {
   	(void)fprintf(stderr,"\nusage:  secpass file.g objects\n\n");
   }

   else
   {	/*  START # 2  */

	/*  Ask if output goes to standard out or to a file.  */
	(void)fprintf(stdout,"Write output to standard out (0) or a file(1) ");
	(void)fprintf(stdout,"not at all (2)?  ");
	fflush(stdout);
	scanf("%d",&iwrite);
	if((iwrite != 0) && (iwrite != 1)) iwrite=2;
	if(iwrite == 1)
	{
	   (void)fprintf(stdout,"Enter name of output file (15 char max).  ");
	   fflush(stdout);
	   scanf("%s",filename);
	   fp3 = fopen(filename,"w");
	}

	/*  Which file that has second pass information in it?  */
	(void)fprintf(stdout,"Enter name of file that has second pass ");
	(void)fprintf(stdout,"information\nin it (15 char max).  ");
	fflush(stdout);
	scanf("%s",spfile);

	/*  Ask for type of output file to be generated.  */
	(void)printf("Enter type of output file to be generated.\n");
	(void)printf("\t 0 - PRISM File\n");
	(void)printf("\t 1 - Generic File\n");
	(void)fflush(stdout);
	(void)scanf("%d",&typeout);

	/*  Read name of file to write conductivity information  */
	/*  to for use in PRISM.  */
	if(typeout == 0)
	{
	   (void)fprintf(stdout,"Enter name of file to be created for PRISM ");
	   (void)fprintf(stdout,"conductivity\ninformation (15 char max).  ");
	   fflush(stdout);
	   scanf("%s",confile);
	}

	/*  Read generic file name if necessary.  */
	if(typeout == 1)
	{
	   (void)printf("Enter name of generic file to be created (15 char ");
	   (void)printf("max).  ");
	   (void)fflush(stdout);
	   (void)scanf("%s",genfile);
	}

	/*  Which calculated length should be used when writing to  */
	/*  this file:  1 -> average length, 2 -> rms length, 3 ->  */
	/*  minimum length, and 4 -> maximum length.  */
	(void)fprintf(stdout,"Which length calculation should be used when\n");
	(void)fprintf(stdout,"computing conduction\nbetween regions?\n");
	(void)fprintf(stdout,"\t1 - average length\n");
	(void)fprintf(stdout,"\t2 - rms length\n");
	(void)fprintf(stdout,"\t3 - minimum length\n");
	(void)fprintf(stdout,"\t4 - maximum length\n");
	fflush(stdout);
	scanf("%d",&itype);

	/*  Read name of file to write conductivity information to  */
	/*  in table format.  */
	(void)fprintf(stdout,"Enter name of file to be created for ");
	(void)fprintf(stdout,"conductivity\ntable (15 char max).  ");
	fflush(stdout);
	scanf("%s",tblfile);

	/*  Read name of material file that contains thermal  */
	/*  conductivity information.  */
	(void)fprintf(stdout,"Enter name of material file (15 char max).  ");
	(void)fflush(stdout);
	(void)scanf("%s",filemat);

	/*  Read name of error file.  */
	(void)fprintf(stdout,"Enter name of error file to be created ");
	(void)fprintf(stdout,"(15 char max).  ");
	fflush(stdout);
	scanf("%s",fileerr);

	/*  Write out file information.  */
	if(iwrite ==1)
	{
	   (void)fprintf(fp3,"\nsecond pass file:  %s\n",spfile);
	   (void)fprintf(fp3,"material file:  %s\n",filemat);
	   if(typeout == 0)
	   {
		(void)fprintf(fp3,"conductivity file for use ");
	   	(void)fprintf(fp3,"with PRISM:  %s\n",confile);
	   }
	   if(typeout == 1) (void)fprintf(fp3,"generic file:  %s\n",genfile);
	   fflush(fp3);
	   if(itype == 1) (void)fprintf(fp3,"\taverage length being used\n");
	   if(itype == 2) (void)fprintf(fp3,"\trms length being used\n");
	   if(itype == 3) (void)fprintf(fp3,"\tminimum length being used\n");
	   if(itype == 4) (void)fprintf(fp3,"\tmaximum length being used\n");
	   (void)fprintf(fp3,"conductivity table file:  %s\n",tblfile);
	   (void)fprintf(fp3,"error file:  %s\n\n",fileerr);
	   fflush(fp3);
	}

	/*  Read thermal conductivity file.  */
	fp5 = fopen(filemat,"r");
	for(i=0; i<41; i++)
	{
	   (void)fgets(line,151,fp5);
	   (void)sscanf(line,"%*d%*f%*f%*f%*f%lf",&k[i]);
	}

	/*  Print out the thermal conductivity.  */
/*
 *	for(i=0; i<41; i++)
 *	{
 *	   (void)fprintf(stdout,"  thermal conductivity %d = %f\n",i,k[i]);
 *	   (void)fflush(stdout);
 *	}
 */

	/*  Build the directory.  */
	index = 1;	/*  Set index for rt_dirbuild.  */
	rtip=rt_dirbuild(argv[index],idbuf,sizeof(idbuf));
	(void)fprintf(stdout,"Database title:  %s\n",idbuf);
	fflush(stdout);

	/*  Set useair to 1, to show hits of air.  */
	rtip->useair = 1;

	/*  Load desired objects.  */
	index = 2;	/*  Set index for rt_gettree.  */
	while(argv[index] != NULL)
	{
	   rt_gettree(rtip,argv[index]);
	   index += 1;
	}

	/*  Find total number of regions in mged file.  */
	nmged = (int)rtip->nregions;

	(void)fprintf(stdout,"Number of regions in mged file:  %d\n",nmged);
	(void)fflush(stdout);

	if(iwrite == 1)
	{
	  (void)fprintf(stdout,fp3,"Number of regions in mged file:  %d\n",nmged);
	  (void)fflush(fp3);
	}

	/*  Number of regions known, everything can be malloced.  */
	(void)fprintf(stdout,"Mallocing arrays.\n");
	(void)fflush(stdout);

	cond = malloc( nmged * sizeof(struct table) );
	(void)fprintf(stdout,"cond malloced\n");
	(void)fflush(stdout);
	for(i=0; i<nmged; i++)
	{
		cond[i].shrarea = malloc( nmged * sizeof(double) );
		cond[i].avglen = malloc( nmged * sizeof(double) );
		cond[i].rmslen = malloc( nmged * sizeof(double) );
		cond[i].minlen = malloc( nmged * sizeof(double) );
		cond[i].maxlen = malloc( nmged * sizeof(double) );
		cond[i].numcal = malloc( nmged * sizeof(double) );
		cond[i].rkavg = malloc( nmged * sizeof(double) );
		cond[i].rkrms = malloc( nmged * sizeof(double) );
		cond[i].rkmin = malloc( nmged * sizeof(double) );
		cond[i].rkmax = malloc( nmged * sizeof(double) );
	}
	(void)fprintf(stdout,"loop malloced\n");
	(void)fflush(stdout);

	/*  All variables 'dimensioned', now zero all variables.  */
	for(i=0; i<nmged; i++)
	{
		cond[i].centroid[0] = (double)0.;
		cond[i].centroid[1] = (double)0.;
		cond[i].centroid[2] = (double)0.;
		cond[i].mat = (int)0;
		for(j=0; j<nmged; j++)
		{
		   cond[i].shrarea[j] = (double)0.;
		   cond[i].avglen[j] = (double)0.;
		   cond[i].rmslen[j] = (double)0.;
		   cond[i].minlen[j] = (double)0.;
		   cond[i].maxlen[j] = (double)0.;
		   cond[i].numcal[j] = (double)0.;
		   cond[i].rkavg[j] = (double)0.;
		   cond[i].rkrms[j] = (double)0.;
		   cond[i].rkmin[j] = (double)0.;
		   cond[i].rkmax[j] = (double)0.;
		}
	}
	(void)fprintf(stdout,"All variables zeroed.\n");
	(void)fflush(stdout);

	/*  Now open file with second pass information in it.  */
	fp = fopen(spfile,"r");
	(void)fprintf(stdout,"second pass file opened\n");
	(void)fflush(stdout);

	/*  Read number of regions in file.  */
	fscanf(fp,"%d\n",&numreg);
	(void)fprintf(stdout,"The number of regions read was %d\n",numreg);
	(void)fflush(stdout);

	if(iwrite == 1)
	{
	   (void)fprintf(fp3,"number of regions read from second ");
	   (void)fprintf(fp3,"pass file:  %d\n",numreg);
	   fflush(fp3);
	}

	/*  Read all information in file.  */
	for(i=0; i<numreg; i++)
	{
	   fscanf(fp,"%*d%le%le%le%d\n",&cond[i].centroid[0],
	                              &cond[i].centroid[1],
	                              &cond[i].centroid[2],&cond[i].mat);
/*
 *	   (void)fprintf(stdout,"reg=%8d, centroid:  %10.0f, %10.0f, %10.0f\n",
 *		i,cond[i].centroid[0],cond[i].centroid[1],cond[i].centroid[2]);
 *	   fflush(stdout);
 */

	   for(j=0; j<numreg; j++)
	   {
		fscanf(fp,"%*d%le\n",&cond[i].shrarea[j]);
/*
 *		(void)fprintf(stdout,"\treg=%8d, area=%10.0f\n",
 *		   j,cond[i].shrarea[j]);
 *		fflush(stdout);
 */
	   }
	}

	fclose(fp);

	/*  Check that the number of regions in the mged file  */
	/*  and the second pass file are equal.  */
	if(nmged != numreg)
	{
	  (void)fprintf(stdout,"ERROR -- The number of regions in the mged\n");
	  (void)fprintf(stdout,"file (%f) does not equal the number of\n",
		nmged);
	  (void)fprintf(stdout,"regions in the second pass file (%f).\n",
		numreg);
	  (void)fprintf(stdout,"Watch for unexplained errors.\n");
	  (void)fflush(stdout);

	  if(iwrite == 1)
	  {
	    (void)fprintf(stdout,fp3,"ERROR -- The number of regions in the mged\n");
	    (void)fprintf(stdout,fp3,"file (%f) does not equal the number of\n",
		nmged);
	    (void)fprintf(stdout,fp3,"regions in the second pass file (%f).\n",
		numreg);
	    (void)fprintf(stdout,fp3,"Watch for unexplained errors.\n");
	    (void)fflush(fp3);
	  }
	}

	/*  Get database ready by starting preparation.  */
	rt_prep(rtip);

        /*  Find center of bounding rpp.  */
        center[X] = rtip->mdl_min[X] + (rtip->mdl_max[X] -
		rtip->mdl_min[X]) / 2.;
        center[Y] = rtip->mdl_min[Y] + (rtip->mdl_max[Y] -
		rtip->mdl_min[Y]) / 2.;
        center[Z] = rtip->mdl_min[Z] + (rtip->mdl_max[Z] -
		rtip->mdl_min[Z]) / 2.;

        /*  Find length of diagonal.  */
        diagonal = (rtip->mdl_max[X] - rtip->mdl_min[X])
                     * (rtip->mdl_max[X] - rtip->mdl_min[X])
                 + (rtip->mdl_max[Y] - rtip->mdl_min[Y])
                     * (rtip->mdl_max[Y] - rtip->mdl_min[Y])
                 + (rtip->mdl_max[Z] - rtip->mdl_min[Z])
                     * (rtip->mdl_max[Z] - rtip->mdl_min[Z]);
        diagonal = sqrt(diagonal) / 2. + .5;

        /*  Find minimum & maximum of grid.  */
        xmin = center[X] - diagonal;
        xmax = center[X] + diagonal;
        ymin = center[Y] - diagonal;
        ymax = center[Y] + diagonal;
        zmin = center[Z] - diagonal;
        zmax = center[Z] + diagonal;

        /*  Print center of bounding rpp, diagonal, & maximum  */
        /*  & minimum of grid.  */
        (void)fprintf(stdout,"Center of bounding rpp ( %f, %f, %f )\n",
		center[X],center[Y],center[Z]);
        (void)fprintf(stdout,"Length of diagonal of bounding rpp:  %f\n",
		diagonal);
        (void)fprintf(stdout,"Minimums & maximums of grid:\n");
        (void)fprintf(stdout,"  %f - %f\n",xmin,xmax);
        (void)fprintf(stdout,"  %f - %f\n",ymin,ymax);
        (void)fprintf(stdout,"  %f - %f\n\n",zmin,zmax);
        (void)fflush(stdout);

	/*  Write model minimum & maximum.  */
	(void)fprintf(stdout,"Model minimum & maximum.\n");
	(void)fprintf(stdout,"\tX:  %f to %f\n\tY:  %f to %f\n\tZ:  %f to %f\n\n",
	   rtip->mdl_min[X],rtip->mdl_max[X],
	   rtip->mdl_min[Y],rtip->mdl_max[Y],
	   rtip->mdl_min[Z],rtip->mdl_max[Z]);
	fflush(stdout);

	if(iwrite == 1)
	{
	   (void)fprintf(fp3,"Model minimum & maximum.\n");
	   (void)fprintf(fp3,"\tX:  %f to %f\n\tY:  %f kto %f\n",
	      rtip->mdl_min[X],rtip->mdl_max[X],
	      rtip->mdl_min[Y],rtip->mdl_max[Y]);
	   (void)fprintf(fp3,"\tZ:  %f to %f\n\n",
	      rtip->mdl_min[Z],rtip->mdl_max[Z]);
	   fflush(fp3);
	}

	/*  User enters grid spacing.  All units are in mm.  */
	(void)fprintf(stdout,"Enter spacing (mm) between fired rays.  ");
	fflush(stdout);
	scanf("%lf",&gridspace);

	(void)fprintf(stdout,"\ngrid spacing:  %f\n",gridspace);
	fflush(stdout);

	if(iwrite == 1)
	{
	   (void)fprintf(fp3,"gridspacing:  %f\n\n",gridspace);
	   fflush(fp3);
	}

	/*  Set up parameters for rt_shootray.  */
	ap.a_hit = hit;		/*  User supplied hit function.  */
	ap.a_miss = miss;	/*  User supplied miss function.  */
	ap.a_overlap = ovrlap;	/*  user supplied overlap function.  */
	ap.a_rt_i = rtip;	/*  Pointer from rt_dirbuild.  */
	ap.a_onehit = 0;	/*  Hit flag (returns all hits).  */
	ap.a_level = 0;		/*  Recursion level for diagnostics.  */
	ap.a_resource = 0;	/*  Address of resource structure (NULL).  */

	/*  Put angles for rotation into radians.  */
	angle[X] = radians((double)GAMMA);
	angle[Y] = radians((double)BETA);
	angle[Z] = radians((double)ALPHA);

	/*  Set up and shoot down x-axis (positive to negative).  */

	(void)fprintf(stdout,"\nShooting down x-axis.\n");
	fflush(stdout);

	strtpt[X] = xmax;
	strtpt[Y] = ymin + gridspace / 2.;
	strtpt[Z] = zmin + gridspace / 2.;
	strtdir[X] = (-1);
	strtdir[Y] = 0;
	strtdir[Z] = 0;

	/*  Rotate starting point.  (new pt = C + R[P -C])  */
	t[X] = strtpt[X] - center[X];
	t[Y] = strtpt[Y] - center[Y];
	t[Z] = strtpt[Z] - center[Z];

	rotate(t,angle,r);

	ap.a_ray.r_pt[X] = center[X] + r[X];
	ap.a_ray.r_pt[Y] = center[Y] + r[Y];
	ap.a_ray.r_pt[Z] = center[Z] + r[Z];

	/*  Rotate firing direction.  (new dir = R[D])  */
	rotate(strtdir,angle,r);
	ap.a_ray.r_dir[X] = r[X];
	ap.a_ray.r_dir[Y] = r[Y];
	ap.a_ray.r_dir[Z] = r[Z];

	while(strtpt[Z] <= zmax)
	{	/*  START # 3  */

	   iprev = (-1);	/*  No previous shots.  */

	   /*  Call rt_shootray.  */
	   (void)rt_shootray ( &ap );

	   strtpt[Y] += gridspace;
	   if(strtpt[Y] > ymax)
	   {
		strtpt[Y] = ymin + gridspace / 2.;
		strtpt[Z] += gridspace;
	   }

	   t[X] = strtpt[X] - center[X];
	   t[Y] = strtpt[Y] - center[Y];
	   t[Z] = strtpt[Z] - center[Z];

	   rotate(t,angle,r);

	   ap.a_ray.r_pt[X] = center[X] + r[X];
	   ap.a_ray.r_pt[Y] = center[Y] + r[Y];
	   ap.a_ray.r_pt[Z] = center[Z] + r[Z];

	}	/*  END # 3  */


	/*  Calculate final length between centroid & shared surface area.  */
	if(iwrite == 0)
	{
		(void)fprintf(stdout,"\n\nFinal numbers.\n");
		fflush(stdout);
	}

	for(i=0; i<numreg; i++)
	{

	   if(iwrite == 0)
	   {
	   	(void)fprintf(stdout,"reg#=%d, matid=%d\n",i,cond[i].mat);
	   	fflush(stdout);
	   }

	   if(iwrite == 1)
	   {
		(void)fprintf(fp3,"reg#=%d, matid=%d\n",i,cond[i].mat);
		fflush(fp3);
	   }

	   for(j=0; j<numreg; j++)
	   {
		if(cond[i].numcal[j] > ZEROTOL)
		{
		   cond[i].avglen[j] /= cond[i].numcal[j];
		   cond[i].rmslen[j] /= cond[i].numcal[j];
		   cond[i].rmslen[j] = sqrt(cond[i].rmslen[j]);

		   if(iwrite == 0)
		   {
		      (void)fprintf(stdout,"\tadjreg=%d, numcal=%f, shrarea=%f, ",
		         j,cond[i].numcal[j],cond[i].shrarea[j]);
		      (void)fprintf(stdout,"avglen=%f\n",cond[i].avglen[j]);
		      (void)fprintf(stdout,"\t\trmslen=%f, ",cond[i].rmslen[j]);
		      (void)fprintf(stdout,"minlen=%f, maxlen=%f\n",
		         cond[i].minlen[j],cond[i].maxlen[j]);
		      fflush(stdout);
		   }

		   if(iwrite == 1)
		   {
		      (void)fprintf(fp3,"\tadjreg=%d, numcal=%f, shrarea=%f, ",
		         j,cond[i].numcal[j],cond[i].shrarea[j]);
		      (void)fprintf(fp3,"avglen=%f\n",cond[i].avglen[j]);
		      (void)fprintf(fp3,"\t\trmslen=%f, ",cond[i].rmslen[j]);
		      (void)fprintf(fp3,"minlen=%f, maxlen=%f\n",
		         cond[i].minlen[j],cond[i].maxlen[j]);
		      fflush(fp3);
		   }

		}
		else
		{
		   cond[i].avglen[j] = 0.;
		   cond[i].rmslen[j] = 0.;
		}
	   }
	}

	if(iwrite == 1)
	{
	   /*  Print summary of all files used.  */
	   (void)fprintf(fp3,"\n\nSUMMARY OF FILES USED & CREATED\n");
	   (void)fprintf(fp3,"\t.g file used:  %s\n",argv[1]);
	   (void)fprintf(fp3,"\tregions used:\n");
	   fflush(fp3);
	   i=2;
	   while(argv[i] != NULL)
	   {
	      (void)fprintf(fp3,"\t\t%s\n",argv[i]);
	      fflush(fp3);
	      i++;
	   }
	   (void)fprintf(fp3,"\tfile containing second pass information:  %s\n",
		spfile);
	   (void)fprintf(fp3,"\tmaterial file used:  %s\n",filemat);
	   (void)fprintf(fp3,"\toutput file created:  %s\n",filename);
	   if(itype == 0) (void)fprintf(fp3,"\tconductivity file created:  %s\n"
		,confile);
	   if(itype == 1) (void)fprintf(fp3,"\tgeneric file created:  %s\n"
		,genfile);
	   (void)fprintf(fp3,"\tconductivity table file created:  %s\n",
		tblfile);
	   (void)fprintf(fp3,"\terror file created:  %s\n\n\n",fileerr);
	   fflush(fp3);

	   fclose(fp3);
	}

	/*  Open conductivity file to be used with PRISM if needed.  */
	if(typeout == 0)
	{
	   fp1=fopen(confile,"w");
	   (void)fprintf(fp1,"Conductivity file for use with PRISM.\n");
	   fflush(fp1);
	}

	/*  Make calculations & write to conductivity file.  */
	for(i=0; i<numreg; i++)
	{	/*  START # 6  */

	/*  Make conductivity file triangular.  This program still  */
	/*  computes the ENTIRE matrix.  */

	   for(j=(i+1); j<numreg; j++)
	   {	/*  START # 7  */

		if( (cond[i].avglen[j] != 0) )
		{	/*  START # 8  */
		   /*  Find correct thermal conductivity.  */
		   /*  If ki or kj = 0 => rk = 0.  */
		   ki = k[cond[i].mat];
		   kj = k[cond[j].mat];

		   /*  All calculations will be in meters &  */
		   /*  square meters.  */

		   areai = cond[i].shrarea[j] * 1.e-6;
		   areaj = cond[j].shrarea[i] * 1.e-6;

		   /*  average length  */
		   leni = cond[i].avglen[j] * 1.e-3;
		   lenj = cond[j].avglen[i] * 1.e-3;
		   if(((-ZEROTOL < ki) && (ki < ZEROTOL)) ||
		      ((-ZEROTOL < kj) && (kj < ZEROTOL)))
			cond[i].rkavg[j] = 0.;
		   else
		   {
			rki = leni / (ki * areai);
			rkj = lenj / (kj * areai);
		        cond[i].rkavg[j] = 1. / (rki + rkj);
		   }

		   /*  rms length  */
		   leni = cond[i].rmslen[j] * 1.e-3;
		   lenj = cond[j].rmslen[i] * 1.e-3;
		   if(((-ZEROTOL < ki) && (ki < ZEROTOL)) ||
		      ((-ZEROTOL < kj) && (kj < ZEROTOL)))
			cond[i].rkrms[j] = 0.;
		   else
		   {
		        rki = leni / (ki * areai);
		        rkj = lenj / (kj * areai);
		        cond[i].rkrms[j] = 1. / (rki + rkj);
		   }

		   /*  minimum length  */
		   leni = cond[i].minlen[j] * 1.e-3;
		   lenj = cond[j].minlen[i] * 1.e-3;
		   if(((-ZEROTOL < ki) && (ki < ZEROTOL)) ||
		      ((-ZEROTOL < kj) && (kj < ZEROTOL)))
			cond[i].rkmin[j] = 0.;
		   else
		   {
		        rki = leni / (ki * areai);
		        rkj = lenj / (kj * areai);
		        cond[i].rkmin[j] = 1. / (rki + rkj);
		   }

		   /*  maximum length  */
		   leni = cond[i].maxlen[j] * 1.e-3;
		   lenj = cond[j].maxlen[i] * 1.e-3;
		   if(((-ZEROTOL < ki) && (ki < ZEROTOL)) ||
		      ((-ZEROTOL < kj) && (kj < ZEROTOL)))
			cond[i].rkmax[j] = 0.;
		   else
		   {
		        rki = leni / (ki * areai);
		        rkj = lenj / (kj * areai);
		        cond[i].rkmax[j] = 1. / (rki + rkj);
		   }

		   /*  Print if had adjacent regions, conductivity  */
		   /*  may be zero.  */

		   /*  Print only if PRISM file is to be created.  */
		   if(typeout == 0) {		/*  START # 8A  */
		   if( (itype == 1) && (cond[i].shrarea[j] > ZEROTOL) )
			(void)fprintf(fp1,"%3d %3d %7.3f %.3e\n",
			(i+1),(j+1),cond[i].rkavg[j],
			(cond[i].shrarea[j] * 1.0e-6));

		   if( (itype == 2) && (cond[i].shrarea[j] > ZEROTOL) )
			(void)fprintf(fp1,"%3d %3d %7.3f %.3e\n",
			(i+1),(j+1),cond[i].rkrms[j],
			(cond[i].shrarea[j] * 1.0e-6));

		   if( (itype == 3) && (cond[i].shrarea[j] > ZEROTOL) )
			(void)fprintf(fp1,"%3d %3d %7.3f %.3e\n",
			(i+1),(j+1),cond[i].rkmin[j],
			(cond[i].shrarea[j] * 1.0e-6));

		   if( (itype == 4) && (cond[i].shrarea[j] > ZEROTOL) )
			(void)fprintf(fp1,"%3d %3d %7.3f %.3e\n",
			(i+1),(j+1),cond[i].rkmax[j],
			(cond[i].shrarea[j] * 1.0e-6));

		   fflush(fp1);
		   }				/*  END of # 8A  */
		}	/*  END # 8  */
	   }	/*  END # 7  */
	}	/*  END # 6  */

	if(typeout == 0) fclose(fp1);

	/*  Open and write to generic file if necessary. */
	/*  The format follows.  */
	/*  4  region number  number of adjacent regions  */
	/*     adjacent region  shared area  conduction distance  */

	if(typeout == 1)
	{
	   /*  Open file.  */
	   fp6 = fopen(genfile,"w");
	   (void)printf("Opened generic file.\n");
	   (void)fflush(stdout);

	   for(i=0; i<numreg; i++)
	   {
		/*  Find number of adjacent regions.  */
		(void)printf("Ready to find number of adjacent areas.\n");
		(void)fflush(stdout);
		nadjreg = 0;
		(void)printf("nadjreg = %f\n",nadjreg);
		(void)fflush(stdout);
		for(j=0; j<numreg; j++)
		{
		   (void)printf("%d, %d, %f\n",i,j,cond[i].shrarea[j]);
		   (void)fflush(stdout);
		   if(cond[i].shrarea[j] > ZEROTOL) nadjreg += 1;
		}
		(void)printf("Found number of adjacent areas.\n");
		(void)fflush(stdout);

		(void)fprintf(fp6,"4  %5d  %5d\n",(i+1),nadjreg);
		(void)fflush(fp6);

		for(j=0; j<numreg; j++)
		{
		   if(cond[i].shrarea[j] > ZEROTOL)
		   {
			(void)fprintf(fp6,"   %5d  %.3e  ",(j+1),
			   (cond[i].shrarea[j] * 1.e-6));
			if(itype == 1) (void)fprintf(fp6,"%.3e\n",
			   (cond[i].avglen[j] * 1.e-3));
			if(itype == 2) (void)fprintf(fp6,"%.3e\n",
			   (cond[i].rmslen[j] * 1.e-3));
			if(itype == 3) (void)fprintf(fp6,"%.3e\n",
			   (cond[i].minlen[j] * 1.e-3));
			if(itype == 4) (void)fprintf(fp6,"%.3e\n",
			   (cond[i].maxlen[j] * 1.e-3));
		   }
		   (void)fflush(fp6);
		}
	   }
	   fclose(fp6);
	}

	/*  Open conductivity table file and write information to  */
	/*  it.  All units will be in meters or square meters.  */
	fp2=fopen(tblfile,"w");
	(void)fprintf(fp2,"Conductivity table.  Units are in meters or ");
	(void)fprintf(fp2,"square meters.\n");

	(void)fprintf(fp2," reg, mat, adj,   shrarea,");
	(void)fprintf(fp2,"    avglen,     rkavg,");
	(void)fprintf(fp2,"    rmslen,     rkrms,");
	(void)fprintf(fp2,"    minlen,     rkmin,");
	(void)fprintf(fp2,"    maxlen,     rkmax\n");

	fflush(fp2);

	for(i=0; i<numreg; i++)
	{	/*  START # 9  */
	   for(j=0; j<numreg; j++)
	   {	/*  START # 10  */
		if(cond[i].shrarea[j] != 0)
		{	/*  START # 11  */
		   a1 = cond[i].shrarea[j] * 1.e-6;
		   l1 = cond[i].avglen[j] * 1.e-3;
		   l2 = cond[i].rmslen[j] * 1.e-3;
		   l3 = cond[i].minlen[j] * 1.e-3;
		   l4 = cond[i].maxlen[j] * 1.e-3;

		   (void)fprintf(fp2,"%4d,%4d,%4d, %.3e,",
			(i+1),cond[i].mat,(j+1),a1);
		   (void)fprintf(fp2," %.3e, %.3e,",l1,cond[i].rkavg[j]);
		   (void)fprintf(fp2," %.3e, %.3e,",l2,cond[i].rkrms[j]);
		   (void)fprintf(fp2," %.3e, %.3e,",l3,cond[i].rkmin[j]);
		   (void)fprintf(fp2," %.3e, %.3e\n",l4,cond[i].rkmax[j]);

		   fflush(fp2);
		}	/*  END # 11  */
	   }	/*  END # 10  */
	}	/*  END # 9  */

	fclose(fp2);

	/*  Print summary of all files used.  */
	(void)fprintf(stdout,"\n\nSUMMARY OF FILES USED & CREATED\n");
	(void)fprintf(stdout,"\t.g file used:  %s\n",argv[1]);
	(void)fprintf(stdout,"\tregions used:\n");
	fflush(stdout);
	i=2;
	while(argv[i] != NULL)
	{
	   (void)fprintf(stdout,"\t\t%s\n",argv[i]);
	   fflush(stdout);
	   i++;
	}
	(void)fprintf(stdout,"\tfile containing second pass information:  %s\n",
		spfile);
	(void)fprintf(stdout,"\tmaterial file used:  %s\n",filemat);
	if(iwrite == 1)
	{
	   (void)fprintf(stdout,"\toutput file created:  %s\n",filename);
	}
	if(typeout == 0)
	{
	   (void)fprintf(stdout,"\tconductivity file created:  %s\n",confile);
	}
	if(typeout == 1) (void)printf("\tgeneric file created:  %s\n",genfile);
	(void)fprintf(stdout,"\tconductivity table file created:  %s\n",tblfile);
	(void)fprintf(stdout,"\terror file created:  %s\n\n\n",fileerr);
	fflush(stdout);

	/*  Open error file.  */
	fp4 = fopen(fileerr,"w");

	/*  Write errors to error file.  */
	(void)fprintf(fp4,"\nERRORS from secpass\n\n");
	fflush(fp4);
	for(i=0; i<numreg; i++)
	{
	   for(j=0; j<numreg;  j++)
	   {
		if( (cond[i].numcal[j] > ZEROTOL) &&
		   ( cond[i].numcal[j] < MINCAL ) )
		{
		   (void)fprintf(fp4,"region %d, adjacent region %d:\n",
			i,j);
		   (void)fprintf(fp4,"\tnumber of length calculations ");
		   (void)fprintf(fp4,"below minimum of %d\n",MINCAL);
		   fflush(fp4);
		}
	   }
	}

   /*  Everything completed, free memory.  */
   (void)fprintf(stdout,"Freeing memory.\n");
   (void)fflush(stdout);
   for(i=0; i<nmged; i++)
   {
	free(cond[i].shrarea);
	free(cond[i].avglen);
	free(cond[i].rmslen);
	free(cond[i].minlen);
	free(cond[i].maxlen);
	free(cond[i].numcal);
	free(cond[i].rkavg);
	free(cond[i].rkrms);
	free(cond[i].rkmin);
	free(cond[i].rkmax);
   }
   free(cond);

   }	/*  END # 2  */
}	/*  END # 1  */


/*  User supplied hit function.  */

hit(ap,PartHeadp)

register struct application *ap;
struct partition *PartHeadp;

{	/*  START # 1H  */

   register struct partition *pp;
   register struct hit *hitp;
   register struct soltab *stp;
   struct curvature cur;

   double d[3];		/*  used for checking tolerance of  */
			/*  adjacent regions  */
   double dist;		/*  used for finding lenght between  */
			/*  centroid & adjacent surface area  */

/*
 * (void)fprintf(stdout,"In hit function.\n");
 * fflush(stdout);
 */

   pp = PartHeadp->pt_forw;
   for( ; pp != PartHeadp; pp = pp->pt_forw)
   {	/*  START # 2H  */
	icur = pp->pt_regionp->reg_bit;	/*  Number of region hit.  */

	/*  Find hit point of entering ray.  */
	hitp = pp->pt_inhit;
	stp = pp->pt_inseg->seg_stp;
	RT_HIT_NORM(hitp,stp,&(ap->a_ray));
	enterpt[X] = hitp->hit_point[X];
	enterpt[Y] = hitp->hit_point[Y];
	enterpt[Z] = hitp->hit_point[Z];

	/*  Find lengths between centroids and adjacent surface areas.  */

	if(iprev >= 0)
	{	/*  START # 3H  */
	   d[X] = enterpt[X] - leavept[X];
	   if(d[X] < 0) d[X] = (-d[X]);
	   d[Y] = enterpt[Y] - leavept[Y];
	   if(d[Y] < 0) d[Y] = (-d[Y]);
	   d[Z] = enterpt[Z] - leavept[Z];
	   if(d[Z] < 0) d[Z] = (-d[Z]);

	   if( (d[X] < ADJTOL) && (d[Y] < ADJTOL) && (d[Z] < ADJTOL) )
	   {	/*  START # 4H  */
		/*  Find length for previous region. */
		dist = ( (cond[iprev].centroid[X] - enterpt[X])
		         * (cond[iprev].centroid[X] - enterpt[X]) ) +
		       ( (cond[iprev].centroid[Y] - enterpt[Y])
		         * (cond[iprev].centroid[Y] - enterpt[Y]) ) +
		       ( (cond[iprev].centroid[Z] - enterpt[Z])
		         * (cond[iprev].centroid[Z] - enterpt[Z]) );
		dist = sqrt(dist);
		cond[iprev].avglen[icur] += dist;
		cond[iprev].rmslen[icur] += (dist * dist);
		cond[iprev].numcal[icur] += 1.;

		if( (-ZEROTOL < cond[iprev].minlen[icur]) &&
		    (cond[iprev].minlen[icur] < ZEROTOL) )
		{
			cond[iprev].minlen[icur] = dist;
		}
		else if( dist < cond[iprev].minlen[icur] )
		{
			cond[iprev].minlen[icur] = dist;
		}

		if( (-ZEROTOL < cond[iprev].maxlen[icur]) &&
		    (cond[iprev].maxlen[icur] < ZEROTOL) )
		{
			cond[iprev].maxlen[icur] = dist;
		}
		else if( cond[iprev].maxlen[icur] < dist )
		{
			cond[iprev].maxlen[icur] = dist;
		}

/*
 *		(void)fprintf(stdout,"\treg#:  %d, length:  %f\n",iprev,dist);
 *		fflush(stdout);
 */

		/*  Find lenght for current region.  */
		dist = ( (cond[icur].centroid[X] - enterpt[X])
		         * (cond[icur].centroid[X] - enterpt[X]) ) +
		       ( (cond[icur].centroid[Y] - enterpt[Y])
		         * (cond[icur].centroid[Y] - enterpt[Y]) ) +
		       ( (cond[icur].centroid[Z] - enterpt[Z])
		         * (cond[icur].centroid[Z] - enterpt[Z]) );
		dist = sqrt(dist);
		cond[icur].avglen[iprev] += dist;
		cond[icur].rmslen[iprev] += (dist * dist);
		cond[icur].numcal[iprev] += 1.;

		if( (-ZEROTOL < cond[icur].minlen[iprev]) &&
		    (cond[icur].minlen[iprev] < ZEROTOL) )
		{
			cond[icur].minlen[iprev] = dist;
		}
		else if( dist < cond[icur].minlen[iprev] )
		{
			cond[icur].minlen[iprev] = dist;
		}

		if( (-ZEROTOL < cond[icur].maxlen[iprev]) &&
		    (cond[icur].maxlen[iprev] < ZEROTOL) )
		{
			cond[icur].maxlen[iprev] = dist;
		}
		else if( cond[icur].maxlen[iprev] < dist )
		{
			cond[icur].maxlen[iprev] = dist;
		}

/*
 *		(void)fprintf(stdout,"\treg#:  %d, length:  %f\n",icur,dist);
 *		fflush(stdout);
 */

	   }	/*  END # 4H  */
	}	/*  END # 3H  */

	/*  Find hit point of leaving ray.  */
	hitp = pp->pt_outhit;
	stp = pp->pt_outseg->seg_stp;
	RT_HIT_NORM(hitp,stp,&(ap->a_ray));
	leavept[X] = hitp->hit_point[X];
	leavept[Y] = hitp->hit_point[Y];
	leavept[Z] = hitp->hit_point[Z];

/*
 *	(void)fprintf(stdout,"current region:  %d, previous region:  %d\n",icur,iprev);
 *	(void)fprintf(stdout,"   entering pt:  %f, %f, %f\n",enterpt[X],enterpt[Y],
 *		enterpt[Z]);
 *	(void)fprintf(stdout,"   leaving pt:   %f, %f, %f\n",leavept[X],leavept[Y],
 *		leavept[Z]);
 */

	/*  Set previous region to current region.  */
	iprev = icur;
   }	/*  END # 2H  */

   return(1);

}	/*  END # 1H  */


/*  User supplied miss function.  */

miss(ap)

register struct application *ap;

{	/*  START # 1M  */

/*
 * (void)fprintf(stdout,"In miss function.\n");
 * fflush(stdout);
 */

   return(0);

}	/*  END # 1M */


ovrlap(ap,PartHeadp,reg1,reg2)

/*  User supplied overlap function that does nothing.  */

register struct application *ap;
struct partition *PartHeadp;
struct region *reg1,*reg2;
{
	return(1);
}


