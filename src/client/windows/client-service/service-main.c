/* Copyright (C) 2010 Omnibond, Inc. */

/* Client Service - service control functions */

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "client-service.h"
#include "config.h"
#include "fs.h"

#define WIN32ServiceName           "orangefs-client"
#define WIN32ServiceDisplayName    "OrangeFS Client"

/* globals */
SERVICE_STATUS_HANDLE hstatus;
SERVICE_STATUS service_status;

#ifdef _DEBUG
BOOL debug = TRUE;
#else
BOOL debug = FALSE;
#endif

int is_running = 0;
int run_service = 0;  

HANDLE hthread;

DWORD thread_start(PORANGEFS_OPTIONS options);
DWORD thread_stop();

DWORD WINAPI main_loop(LPVOID poptions);

FILE *debug_log = NULL;

/* externs */
extern int __cdecl dokan_loop(PORANGEFS_OPTIONS options);

void init_service_log()
{
    char exe_path[MAX_PATH], *p;
    int ret;

    if (!debug || !run_service)
        return;

    /* create log file in exe directory */
    ret = GetModuleFileName(NULL, exe_path, MAX_PATH);
    if (ret != 0)
    {
        /* get directory */
        p = strrchr(exe_path, '\\');
        if (p)
            *p = '\0';

        strcat(exe_path, "\\service.log");

        debug_log = fopen(exe_path, "a");
    }
}

void service_debug(char *format, ...)
{
    char buffer[512];
    va_list argp;

    if (!debug || !run_service)
        return;

    va_start(argp, format);
    vsprintf_s(buffer, sizeof(buffer), format, argp);
    va_end(argp);

    fprintf(debug_log, buffer);
    fflush(debug_log);

}

void close_service_log()
{
    if (!debug || !run_service)
        return;

    if (debug_log)
    {
        fprintf(debug_log, "\n");
        fclose(debug_log);
    }
}

BOOL check_mount_point(const char *mount_point)
{
    const char *slash;
    char drive;
    DWORD mask;

    /* first check if a directory rather than drive is mapped */
    slash = strchr(mount_point, '\\');
    if (slash && slash[1] != '\0')
        /* Dokan will exit if directory is invalid */
        return TRUE;

    drive = toupper(mount_point[0]);
    drive -= 'A';
    if (drive < 0 || drive > 25)
        return FALSE;

    mask = GetLogicalDrives();
    if (mask == 0)
    {
        fprintf(stderr, "GetLogicalDrives failed: %u\n", GetLastError());
        return FALSE;
    }

    return !(mask & (1 << drive));
}

DWORD service_install()
{
    SC_HANDLE sch_service;
    SC_HANDLE sch_manager;
    char *exe_path, *command;
    size_t size;
    int err;

    /* Get location of executable */
    size = 512;
    do {
        exe_path = (char *) malloc(size);
        if (!exe_path) 
        {
            fprintf(stderr, "Insufficient memory\n");
            return -1;
        }

        GetModuleFileName(NULL, exe_path, size);
        err = GetLastError();

        if (err == ERROR_INSUFFICIENT_BUFFER)
        {
            free(exe_path);
            size *= 2;
        }
        else if (err != ERROR_SUCCESS)
        {
            free(exe_path);
            fprintf(stderr, "Error: GetModuleFileName (%u)\n", err);
            return -1;
        }

    } while (err == ERROR_INSUFFICIENT_BUFFER);

    /* append -service option to command */
    command = (char *) malloc(strlen(exe_path) + 16);
    if (!command)
    {
        fprintf(stderr, "Insufficient memory\n");
        return -1;
    }
    strcpy(command, exe_path);
    strcat(command, " -service");

    /* open the service manager */
    sch_manager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (sch_manager != NULL)
    {
        /* create the service */
        sch_service = CreateService(
            sch_manager,
            WIN32ServiceName,
            WIN32ServiceDisplayName,
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            command,
            NULL,
            NULL,
            TEXT("\0\0"),
            NULL,
            NULL);

        if (sch_service != NULL)
        {
            printf("%s installed\n", WIN32ServiceDisplayName);
            CloseServiceHandle(sch_service);
        }
        else
        {
            fprintf(stderr, "Error: CreateService (%u)\n", GetLastError());
            return -1;
        }

        CloseServiceHandle(sch_manager);
    }
    else
    {
        fprintf(stderr, "Error: OpenSCManager (%u)\n", GetLastError());
        return -1;
    }

    free(exe_path);
    free(command);

    return 0;
}

DWORD service_remove()
{
    SC_HANDLE sch_service;
    SC_HANDLE sch_manager;

    /* open service manager */
    sch_manager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (sch_manager != NULL)
    {
        /* open service */
        sch_service = OpenService(sch_manager, WIN32ServiceName, 
                        SERVICE_ALL_ACCESS | DELETE);
        if (sch_service != NULL)
        {
            /* attempt to stop service */
            if (ControlService(sch_service, SERVICE_CONTROL_STOP,
                  &service_status)) 
            {
                Sleep(1000);

                while (QueryServiceStatus(sch_service, &service_status))
                {
                    if (service_status.dwCurrentState == SERVICE_STOP_PENDING)                    
                        Sleep(1000);
                    else
                        break;
                }

            }
            
            if (DeleteService(sch_service))                
                printf("%s removed\n", WIN32ServiceDisplayName);
            else
                fprintf(stderr, "Error: DeleteService (%u)\n", GetLastError());

            CloseServiceHandle(sch_service);

        }
        else
        {
            fprintf(stderr, "Error: OpenService (%u)\n", GetLastError());
            return -1;
        }

        CloseServiceHandle(sch_manager);
    }
    else
    {
        fprintf(stderr, "Error: OpenSCManager (%u)\n", GetLastError());
        return -1;
    }

    return 0;

}

/* service control handler */
void WINAPI service_ctrl(DWORD ctrl_code)
{
    switch (ctrl_code)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        service_debug("service_ctrl: shutdown received\n");
        
        service_status.dwCurrentState = SERVICE_STOP_PENDING;
        Sleep(1000);
        SetServiceStatus(hstatus, &service_status);
        
        is_running = 0;
        thread_stop();
    }
}

void WINAPI service_main(DWORD argc, char *argv[])
{
    PORANGEFS_OPTIONS options;

    /* allocate options */
    options = (PORANGEFS_OPTIONS) calloc(sizeof(ORANGEFS_OPTIONS), 1);

    /* default mount point */
    strcpy(options->mount_point, "Z:");

    /* read from config file */
    get_config(options);

#ifndef _DEBUG
    debug = options->debug;
#endif

    init_service_log();

    service_debug("Entered service_main\n");

    if (!check_mount_point(options->mount_point))
        return;

    /* register our control handler routine */
    if ((hstatus = RegisterServiceCtrlHandler(WIN32ServiceName, service_ctrl))
           != NULL)
    {
        service_debug("Service registered\n");

        /* run the service */
        service_status.dwCurrentState = SERVICE_RUNNING;
        service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
        service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        service_status.dwWin32ExitCode = NO_ERROR;
        service_status.dwServiceSpecificExitCode = 0;

        /* execute service main loop */
        if (SetServiceStatus(hstatus, &service_status))
        {
            is_running = 1;
            service_debug("Starting thread\n");
            thread_start(options);
        }
        
        /* shut down service */        
        service_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(hstatus, &service_status);        
    }
    else
    {
        service_debug("RegisterServiceCtrlHandler failed: %u\n", GetLastError());
        /* TODO: error reporting */
    }
    
    close_service_log();

}

DWORD thread_start(PORANGEFS_OPTIONS options)
{
    DWORD err = 0;

    service_debug("thread_start enter\n");

    /* create and run the new thread */
    hthread = CreateThread(NULL, 
                           0, 
                           main_loop,
                           options,
                           0,
                           NULL);
    if (hthread)
    {  
        WaitForSingleObject(hthread, INFINITE);
    }
    else
    {
        err = GetLastError();
        service_debug("CreateThread failed: %u\n", err);
    }

    service_debug("thread_start exit\n");

    return err;                           
}

DWORD thread_stop()
{
    DWORD err = 0;

    service_debug("thread_stop enter\n");
    
    /* stop the thread */
    if (!TerminateThread(hthread, 0))
    {
        err = GetLastError();
        service_debug("TerminateThread failed: %u\n", err);
    }

    service_debug("thread_stop exit\n");

    return err;
}

DWORD WINAPI main_loop(LPVOID poptions)
{
    PORANGEFS_OPTIONS options = (PORANGEFS_OPTIONS) poptions;
    char *tabfile, exe_path[MAX_PATH], *p;
    FILE *f;
    int ret, malloc_flag = 0;

    /* locate tabfile -- env. variable overrides */
    if (!(tabfile = getenv("PVFS2TAB_FILE")))
    {
        ret = GetModuleFileName(NULL, exe_path, MAX_PATH);
        if (ret)
        {
            /* get directory */
            p = strrchr(exe_path, '\\');
            if (p)
                *p = '\0';

            tabfile = (char *) malloc(MAX_PATH);
            malloc_flag = TRUE;

            strcpy(tabfile, exe_path);
            strcat(tabfile, "\\orangefstab");

            /* attempt to open file */
            f = fopen(tabfile, "r");
            if (f)
                fclose(f);
            else 
            {
                /* switch to pvfs2tab -- fs_initialize will fail if not valid */
                strcpy(tabfile, exe_path);
                strcat(tabfile, "\\pvfs2tab");
            }
        }
        else
        {
           fprintf(stderr, "GetModuleFileName failed: %u\n", GetLastError());
        }
    }

    /* init file systems */
    if (tabfile)
    {
        service_debug("Using tabfile: %s\n", tabfile);
        ret = fs_initialize(tabfile);
    }
    else
        ret = ERROR_FILE_NOT_FOUND;

    /* run dokan operations */
    if (ret == 0)
    {
        dokan_loop(options);

        /* close file systems */
        fs_finalize();
    }
    else 
    {
        service_debug("fs_initialize returned %d\n", ret);
        fprintf(stderr, "fs_initialize returned %d\n", ret);
    }

    if (malloc_flag)
        free(tabfile);

    return (DWORD) ret;
}

int main(int argc, char **argv, char **envp)
{
  int i = 0;
  PORANGEFS_OPTIONS options;
  DWORD err = 0;

  SERVICE_TABLE_ENTRY dispatch_table[2] = 
  {
      {WIN32ServiceName, (LPSERVICE_MAIN_FUNCTION) service_main},
      {NULL, NULL}
  };

  options = (PORANGEFS_OPTIONS) calloc(sizeof(ORANGEFS_OPTIONS), 1);

  /* default mount point */
  strcpy(options->mount_point, "Z:");

  /* get options from config file */
  get_config(options);

  /* command line arguments override config file options */
  for (i = 1; i < argc; i++) 
  {
      if (!stricmp(argv[i], "-installService") ||
          !stricmp(argv[i], "-w") || !stricmp(argv[i], "/w")) 
      {
          return service_install();
      }
      
      if (!stricmp(argv[i], "-removeService") ||
               !stricmp(argv[i], "-u") || !stricmp(argv[i], "/u"))
      {
          return service_remove();
      }
      
      if (!strcmp(argv[i], "-service"))
      {
          run_service = 1;
      }

      if (!strcmp(argv[i], "-mount") || !strcmp(argv[i], "-m") ||
          !strcmp(argv[i], "/m"))
      {
          if (i < (argc - 1))
              strncpy(options->mount_point, argv[++i], MAX_PATH);
          else
              fprintf(stderr, "Invalid argument -mount. Using mount point Z:\n");
      }

      /* debug is always enabled for debug version */
#ifndef _DEBUG
      if (!strcmp(argv[i], "-debug") || !strcmp(argv[i], "-d") ||
          !strcmp(argv[i], "/d"))
          debug = TRUE;
#endif
  }

  options->debug = debug;

  if (run_service) 
  {
      /* dispatch the main service thread */
      StartServiceCtrlDispatcher(dispatch_table);
  } 
  else 
  {    
      is_running = 1;

      if (!check_mount_point(options->mount_point))
      {
          fprintf(stderr, "Drive already in use\n");
          return -1;
      }

      err = main_loop(options);
      printf("main_loop exited: %d\n", err);
  }

  free(options);

  return err;
}