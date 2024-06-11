#include "sta/Sta.hh"
#include "sta/StaMain.hh"
#include "sta/StaConfig.hh"

#include <stdio.h>
#include <cstdlib> 
#include <tcl.h>

namespace sta 
{
  extern const char *tcl_inits[];
}

using std::string;
using sta::stringEq;
using sta::findCmdLineFlag;
using sta::Sta;
using sta::initSta;
using sta::evalTclInit;
using sta::sourceTclFile;
using sta::parseThreadsArg;
using sta::tcl_inits;
using sta::is_regular_file;

// Swig uses C linkage for init functions.
extern "C" 
{
  extern int Sta_Init(Tcl_Interp *interp);
}

int cmd_argc;
char **cmd_argv;
const char *init_filename = ".sta";

void initStaApp(int &argc,	char *argv[],	Tcl_Interp *interp)
{
  initSta();
  Sta *sta = new Sta;
  Sta::setSta(sta);
  sta->makeComponents();
  sta->setTclInterp(interp);
  int thread_count = parseThreadsArg(argc, argv);
  sta->setThreadCount(thread_count);

  // Define swig TCL commands.
  Sta_Init(interp);
  // Eval encoded sta TCL sources.
  evalTclInit(interp, tcl_inits);
  Tcl_Eval(interp, "init_sta_cmds");
}


// Tcl init executed inside Tcl_Main.
int staTclAppInit(int argc,	char *argv[],	const char *init_filename,	Tcl_Interp *interp)
{
  // source init.tcl
  if (Tcl_Init(interp) == TCL_ERROR)
    return TCL_ERROR;

  initStaApp(argc, argv, interp);

	// Force No splash
  //if(!findCmdLineFlag(argc, argv, "-no_splash"))
  //  Tcl_Eval(interp, "sta::show_splash");

  if(!findCmdLineFlag(argc, argv, "-no_init")) 
	{
    const char *home = getenv("HOME");
    if(home) 
		{
      string init_path = home;
      init_path += "/";
      init_path += init_filename;
      if (is_regular_file(init_path.c_str()))
        sourceTclFile(init_path.c_str(), true, true, interp);
    }
  }

	// Force exit mode
	bool exit_after_cmd_file = findCmdLineFlag(argc, argv, "-exit");
	if(exit_after_cmd_file == false)
		exit_after_cmd_file = true;

  if(argc > 2 || (argc > 1 && argv[1][0] == '-')) 
    exit(1);
  else 
	{
    if(argc == 2) 
		{
      char *cmd_file = argv[1];
      if (cmd_file) 
			{
	      int result = sourceTclFile(cmd_file, false, false, interp);
        if(exit_after_cmd_file) 
				{
          int exit_code = (result == TCL_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
          exit(exit_code);
        }
      }
    }
  }
  return TCL_OK;
}

int tclAppInit(Tcl_Interp *interp)
{
  return staTclAppInit(cmd_argc, cmd_argv, init_filename, interp);
}

int main(int argc, char *argv[])
{
  cmd_argc = argc;
  cmd_argv = argv;
  Tcl_Main(1, argv, tclAppInit);
  return 0;
}
