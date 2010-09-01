/******************************************************************************
 *
 * mod_gearman - distribute checks with gearman
 *
 * Copyright (c) 2010 Sven Nierlein
 *
 *****************************************************************************/

/* include header */
#include "worker.h"
#include "utils.h"
#include "worker_logger.h"
#include "worker_client.h"

int mod_gm_opt_min_worker      = GM_DEFAULT_MIN_WORKER;
int mod_gm_opt_max_worker      = GM_DEFAULT_MAX_WORKER;
int mod_gm_opt_max_age         = GM_DEFAULT_JOB_MAX_AGE;
int mod_gm_opt_timeout         = GM_DEFAULT_TIMEOUT;

int mod_gm_opt_hosts           = GM_DISABLED;
int mod_gm_opt_services        = GM_DISABLED;
int mod_gm_opt_events          = GM_DISABLED;
int mod_gm_opt_debug_result    = GM_DISABLED;
char *mod_gm_hostgroups_list[GM_LISTSIZE];
char *mod_gm_servicegroups_list[GM_LISTSIZE];

int current_number_of_workers                = 0;
volatile sig_atomic_t current_number_of_jobs = 0;  // must be signal safe


/* work starts here */
int main (int argc, char **argv) {


    /* set signal handlers for a clean exit */
    signal(SIGINT, clean_exit);
    signal(SIGTERM,clean_exit);

    parse_arguments(argc, argv);
    logger( GM_LOG_DEBUG, "main process started\n");

    if(mod_gm_opt_max_worker == 1) {
        worker_client(GM_WORKER_STANDALONE);
        exit( EXIT_SUCCESS );
    }

    setup_child_communicator();

    // create initial childs
    int x;
    for(x=0; x < mod_gm_opt_min_worker; x++) {
        make_new_child();
    }

    // maintain the population
    while (1) {
        // check number of workers every 30 seconds
        // sleep gets canceled anyway when receiving signals
        sleep(30);

        // collect finished workers
        int status;
        while(waitpid(-1, &status, WNOHANG) > 0) {
            current_number_of_workers--;
            logger( GM_LOG_TRACE, "waitpid() %d\n", status);
        }

        if(current_number_of_jobs < 0) { current_number_of_jobs = 0; }
        if(current_number_of_jobs > current_number_of_workers) { current_number_of_jobs = current_number_of_workers; }

        // keep up minimum population
        for (x = current_number_of_workers; x < mod_gm_opt_min_worker; x++) {
            make_new_child();
        }

        int had_to_increase = 0;
        int target_number_of_workers = adjust_number_of_worker(mod_gm_opt_min_worker, mod_gm_opt_max_worker, current_number_of_workers, current_number_of_jobs);
        for (x = current_number_of_workers; x < target_number_of_workers; x++) {
            // top up the worker pool
            make_new_child();
            had_to_increase = 1;
        }

        if(had_to_increase) {
            // wait a little bit, otherwise worker would be spawned really fast
            sleep(1);
        }
    }

    clean_exit(15);
    exit( EXIT_SUCCESS );
}


/* start up new worker */
int make_new_child() {
    logger( GM_LOG_TRACE, "make_new_child()\n");
    pid_t pid = 0;

    /* fork a child process */
    pid=fork();

    /* an error occurred while trying to fork */
    if(pid==-1){
        logger( GM_LOG_ERROR, "fork error\n" );
        return GM_ERROR;
    }

    /* we are in the child process */
    else if(pid==0){
        logger( GM_LOG_DEBUG, "worker started with pid: %d\n", getpid() );

        signal(SIGUSR1, SIG_IGN);
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        // do the real work
        worker_client(GM_WORKER_MULTI);

        logger( GM_LOG_DEBUG, "worker fin: %d\n", getpid() );
        exit(EXIT_SUCCESS);
    }

    /* parent  */
    else if(pid > 0){
        current_number_of_workers++;
    }

    return GM_OK;
}


/* parse command line arguments */
void parse_arguments(int argc, char **argv) {
    int srv_ptr     = 0;
    int srvgrp_ptr  = 0;
    int hostgrp_ptr = 0;

    int set_by_hand = 0;

    while(argc-- != 1) {
        char *ptr;
        char * args   = strdup( argv[1] );
        while ( (ptr = strsep( &args, " " )) != NULL ) {
            char *key   = str_token( &ptr, '=' );
            char *value = str_token( &ptr, 0 );

            if ( key == NULL )
                continue;

            if ( !strcmp( key, "help" ) || !strcmp( key, "--help" )  || !strcmp( key, "-h" ) ) {
                print_usage();
            }

            if ( !strcmp( key, "hosts" ) || !strcmp( key, "--hosts" ) ) {
                set_by_hand++;
                if( value == NULL || !strcmp( value, "yes" ) ) {
                    mod_gm_opt_hosts = GM_ENABLED;
                    logger( GM_LOG_DEBUG, "enabling processing of hosts queue\n");
                }
            }
            else if ( !strcmp( key, "services" ) || !strcmp( key, "--services" ) ) {
                set_by_hand++;
                if( value == NULL || !strcmp( value, "yes" ) ) {
                    mod_gm_opt_services = GM_ENABLED;
                    logger( GM_LOG_DEBUG, "enabling processing of service queue\n");
                }
            }
            else if ( !strcmp( key, "events" ) || !strcmp( key, "--events" ) ) {
                set_by_hand++;
                if( value == NULL || !strcmp( value, "yes" ) ) {
                    mod_gm_opt_events = GM_ENABLED;
                    logger( GM_LOG_DEBUG, "enabling processing of events queue\n");
                }
            }
            else if ( !strcmp( key, "debug-result" ) || !strcmp( key, "--debug-result" ) ) {
                if( value == NULL || !strcmp( value, "yes" ) ) {
                    mod_gm_opt_debug_result = GM_ENABLED;
                    logger( GM_LOG_DEBUG, "adding debug output to check output\n");
                }
            }

            if ( value == NULL )
                continue;

            if ( !strcmp( key, "debug" ) || !strcmp( key, "--debug" ) ) {
                mod_gm_opt_debug_level = atoi( value );
                if(mod_gm_opt_debug_level < 0) { mod_gm_opt_debug_level = 0; }
                logger( GM_LOG_DEBUG, "Setting debug level to %d\n", mod_gm_opt_debug_level );
            }
            else if ( !strcmp( key, "timeout" ) || !strcmp( key, "--timeout" ) ) {
                mod_gm_opt_timeout = atoi( value );
                if(mod_gm_opt_timeout < 1) { mod_gm_opt_timeout = 1; }
                logger( GM_LOG_DEBUG, "Setting default timeout to %d\n", mod_gm_opt_timeout );
            }
            else if ( !strcmp( key, "min-worker" ) || !strcmp( key, "--min-worker" ) ) {
                mod_gm_opt_min_worker = atoi( value );
                if(mod_gm_opt_min_worker <= 0) { mod_gm_opt_min_worker = 1; }
                logger( GM_LOG_DEBUG, "Setting min worker to %d\n", mod_gm_opt_min_worker );
            }
            else if ( !strcmp( key, "max-worker" ) || !strcmp( key, "--max-worker" ) ) {
                mod_gm_opt_max_worker = atoi( value );
                if(mod_gm_opt_max_worker <= 0) { mod_gm_opt_max_worker = 1; }
                logger( GM_LOG_DEBUG, "Setting max worker to %d\n", mod_gm_opt_max_worker );
            }
            else if ( !strcmp( key, "max-age" ) || !strcmp( key, "--max-age" ) ) {
                mod_gm_opt_max_age = atoi( value );
                if(mod_gm_opt_max_age <= 0) { mod_gm_opt_max_age = 1; }
                logger( GM_LOG_DEBUG, "Setting max job age to %d\n", mod_gm_opt_max_age );
            }
            else if ( !strcmp( key, "server" ) || !strcmp( key, "--server" ) ) {
                char *servername;
                while ( (servername = strsep( &value, "," )) != NULL ) {
                    if ( strcmp( servername, "" ) ) {
                        logger( GM_LOG_DEBUG, "Adding server %s\n", servername);
                        mod_gm_opt_server[srv_ptr] = servername;
                        srv_ptr++;
                    }
                }
            }

            else if ( !strcmp( key, "servicegroups" ) || !strcmp( key, "--servicegroups" ) ) {
                char *groupname;
                while ( (groupname = strsep( &value, "," )) != NULL ) {
                    if ( strcmp( groupname, "" ) ) {
                        mod_gm_servicegroups_list[srvgrp_ptr] = groupname;
                        srvgrp_ptr++;
                        logger( GM_LOG_DEBUG, "added seperate worker for servicegroup: %s\n", groupname );
                    }
                }
            }
            else if ( !strcmp( key, "hostgroups" ) || !strcmp( key, "--hostgroups" ) ) {
                char *groupname;
                while ( (groupname = strsep( &value, "," )) != NULL ) {
                    if ( strcmp( groupname, "" ) ) {
                        mod_gm_hostgroups_list[hostgrp_ptr] = groupname;
                        hostgrp_ptr++;
                        logger( GM_LOG_DEBUG, "added seperate worker for hostgroup: %s\n", groupname );
                    }
                }
            }

            else  {
                logger( GM_LOG_ERROR, "unknown option: %s\n", key );
            }
        }
        argv++;
    }

    // did we get any server?
    if(srv_ptr == 0) {
        logger( GM_LOG_ERROR, "please specify at least one server\n" );
        exit(EXIT_FAILURE);
    }

    // nothing set by hand -> defaults
    if(set_by_hand == 0 && srvgrp_ptr == 0 && hostgrp_ptr == 0) {
        logger( GM_LOG_DEBUG, "starting client with default queues\n" );
        mod_gm_opt_hosts    = GM_ENABLED;
        mod_gm_opt_services = GM_ENABLED;
        mod_gm_opt_events   = GM_ENABLED;
    }

    if(srvgrp_ptr == 0 && hostgrp_ptr == 0 && mod_gm_opt_hosts == GM_DISABLED && mod_gm_opt_services == GM_DISABLED && mod_gm_opt_events == GM_DISABLED) {
        logger( GM_LOG_ERROR, "starting client without queues is useless\n" );
        exit(EXIT_FAILURE);
    }

    if(mod_gm_opt_min_worker > mod_gm_opt_max_worker)
        mod_gm_opt_min_worker = mod_gm_opt_max_worker;

}


/* print usage */
void print_usage() {
    printf("usage:\n");
    printf("\n");
    printf("worker [ --debug=<lvl>         ]\n");
    printf("       [ --debug-result        ]\n");
    printf("       [ --help|-h             ]\n");
    printf("\n");
    printf("       [ --server=<server>     ]\n");
    printf("\n");
    printf("       [ --hosts               ]\n");
    printf("       [ --services            ]\n");
    printf("       [ --events              ]\n");
    printf("       [ --hostgroup=<name>    ]\n");
    printf("       [ --servicegroup=<name> ]\n");
    printf("\n");
    printf("       [ --min-worker=<nr>     ]\n");
    printf("       [ --max-worker=<nr>     ]\n");
    printf("\n");
    printf("       [ --max-age=<sec>       ]\n");
    printf("       [ --timeout             ]\n");
    printf("\n");
    printf("\n");

    exit( EXIT_SUCCESS );
}

/* check child signal pipe */
void check_signal(int sig) {
    logger( GM_LOG_TRACE, "check_signal(%i)\n", sig);

    int shmid;
    int *shm;

    // Locate the segment.
    if ((shmid = shmget(gm_shm_key, GM_SHM_SIZE, 0666)) < 0) {
        perror("shmget");
        exit(1);
    }

    // Now we attach the segment to our data space.
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        exit(1);
    }

    logger( GM_LOG_TRACE, "check_signal: %i\n", shm[0]);
    current_number_of_jobs = shm[0];

    // detach from shared memory
    if(shmdt(shm) < 0)
        perror("shmdt");

    return;
}

/* create shared memory segments */
void setup_child_communicator() {
    logger( GM_LOG_TRACE, "setup_child_communicator()\n");

    // setup signal handler
    struct sigaction usr1_action;
    sigset_t block_mask;
    sigfillset (&block_mask); // block all signals
    usr1_action.sa_handler = check_signal;
    usr1_action.sa_mask    = block_mask;
    usr1_action.sa_flags   = 0;
    sigaction (SIGUSR1, &usr1_action, NULL);

    int shmid;
    int * shm;

    // Create the segment.
    gm_shm_key = getpid(); // use pid as shm key
    if ((shmid = shmget(gm_shm_key, GM_SHM_SIZE, IPC_CREAT | 0666)) < 0) {
        perror("shmget");
        exit(1);
    }

    // Now we attach the segment to our data space.
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        exit(1);
    }
    shm[0] = 0;
    logger( GM_LOG_TRACE, "setup: %i\n", shm[0]);

    // detach from shared memory
    if(shmdt(shm) < 0)
        perror("shmdt");

    return;
}


/* set new number of workers */
int adjust_number_of_worker(int min, int max, int cur_workers, int cur_jobs) {
    int perc_running = (int)cur_jobs*100/cur_workers;
    int idle         = (int)cur_workers - cur_jobs;
    logger( GM_LOG_TRACE, "adjust_number_of_worker(min %d, max %d, worker %d, jobs %d) = %d%% running\n", min, max, cur_workers, cur_jobs, perc_running);
    int target = min;

    if(cur_workers == max)
        return max;

    // > 90% workers running
    if(cur_jobs > 0 && ( perc_running > 90 || idle <= 2 )) {
        // increase target number by 2
        logger( GM_LOG_TRACE, "starting 2 new workers\n");
        target = cur_workers + 2;
    }

    // dont go over the top
    if(target > max) { target = max; }

    return target;
}


/* do a clean exit */
void clean_exit(int sig) {
    logger( GM_LOG_TRACE, "clean_exit(%d)\n", sig);

    // become the process group leader
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_DFL);

    /*
     * send term signal to our childs
     * children will finish the current job and exit
     */
    pid_t pid = getpid();
    logger( GM_LOG_TRACE, "send SIGTERM to %d\n", pid);
    kill(-pid, SIGTERM);
    signal(SIGTERM, SIG_DFL);

    logger( GM_LOG_TRACE, "waiting for childs to exit...\n");
    int status;
    int chld;
    while((chld = wait(&status)) > 0) {
        logger( GM_LOG_TRACE, "wait() %d exited with %d\n", chld, status);
    }

    /*
     * clean up shared memory
     * will be removed when last client detaches
     */
    shmctl( gm_shm_key, IPC_RMID, 0 );

    exit( EXIT_SUCCESS );
}
