/*
	Author:		Gary S. Moss
			U. S. Army Ballistic Research Laboratory
			Aberdeen Proving Ground
			Maryland 21005-5066
			(301)278-6647 or AV-298-6647
*/
#ifndef lint
static char RCSid[] = "@(#)$Header$ (BRL)";
#endif
/*
	Originally extracted from SCCS archive:
		SCCS id:	@(#) execshell.c	2.1
		Modified: 	12/10/86 at 16:04:23	G S M
		Retrieved: 	2/4/87 at 08:53:55
		SCCS archive:	/vld/moss/src/lgt/s.execshell.c
*/

#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include "./extern.h"
#define DFL_SHELL	"/bin/sh"
#define CSH		"/bin/csh"
#define TCSH		"/usr/brl/bin/tcsh"
#define	PERROR_RET() \
	(void) sprintf( error_buf, "\"%s\" (%d)", __FILE__, __LINE__ ); \
	loc_Perror( error_buf ); \
	return	-1;

#include <errno.h>
/* These aren't defined in BSD errno.h.					*/
extern int	errno;
extern int	sys_nerr;
extern char	*sys_errlist[];

void
loc_Perror( msg )
char	*msg;
	{
	if( errno >= 0 && errno < sys_nerr )
		rt_log( "%s: %s\n", msg, sys_errlist[errno] );
	else
		rt_log( "\"%s\" (%d), errno not set, shouldn't call perror.\n",
			__FILE__, __LINE__
			);
	return;
	}

/*	e x e c _ S h e l l ( )
	If args[0] is NULL, spawn a shell, otherwise execute the specified
	command line.
	Return the exit status of the program, or -1 if wait() or fork()
	return an error.
 */
int
exec_Shell( args )
char	*args[];
	{	register int	child_pid;
		static char	error_buf[32];
#if defined( BSD ) || defined( sgi )
		int		(*intr_sig)(), (*quit_sig)();
#else
		void		(*intr_sig)(), (*quit_sig)();
#endif
		register int	i;
	if( args[0] == NULL )
		{ char	*arg_sh = getenv( "SHELL" );
		/* $SHELL, if set, DFL_SHELL otherwise.			*/
		if(	arg_sh == NULL
		/* Work around for process group problem.		*/
		    ||	strcmp( arg_sh, TCSH ) == 0
		    ||	strcmp( arg_sh, CSH ) == 0
			)
			arg_sh = DFL_SHELL;
		args[0] = arg_sh;
		args[1] = NULL;
		}
	intr_sig = signal( SIGINT,  SIG_IGN );
	quit_sig = signal( SIGQUIT, SIG_IGN );
	switch( child_pid = fork() )
		{
		case -1 :
			PERROR_RET();
		case  0 : /* Child process - execute.		*/
			{ int	tmp_fd;
			if(	(tmp_fd =
				open( "/dev/tty", O_WRONLY )) == -1
				)
				{
				PERROR_RET();
				}
			(void) close( 2 );
			if( fcntl( tmp_fd, F_DUPFD, 2 ) == -1 )
				{
				PERROR_RET();
				}
			(void) execvp( args[0], args );
			loc_Perror( args[0] );
			exit( errno );
			}
		default :
			{	register int	pid;
				int		stat_loc;
			while(	(pid = wait( &stat_loc )) != -1
			     && pid != child_pid
				)
				;
			prnt_Event( "\n" );
			(void) signal( SIGINT,  intr_sig );
			(void) signal( SIGQUIT, quit_sig );
			if( pid == -1 )
				{ /* No children.			*/
				loc_Perror( "wait" );
				return	errno;
				}
			switch( stat_loc & 0377 )
				{
				case 0177 : /* Child stopped.		*/
					rt_log(	"\"%s\" (%d) Child stopped.\n",
						__FILE__,
						__LINE__
						);
					return	(stat_loc >> 8) & 0377;
				case 0 :    /* Child exited.		*/
					return	(stat_loc >> 8) & 0377;
				default :   /* Child terminated.	*/
					rt_log(	"\"%s\" (%d) Child terminated, signal %d, status=0x%x.\n",
						__FILE__,
						__LINE__,
						stat_loc&0177,
						stat_loc
						);
					return	stat_loc&0377;
				}
			}
		}
	}
