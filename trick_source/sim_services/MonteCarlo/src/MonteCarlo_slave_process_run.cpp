/*
 * $Id: MonteCarlo_slave_process_run.cpp $
 */

#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>

#include "sim_services/MonteCarlo/include/MonteCarlo.hh"
#include "sim_services/Executive/include/Exec_exception.hh"
#include "sim_services/CommandLineArguments/include/command_line_protos.h"
#include "sim_services/InputProcessor/include/input_processor_proto.h"
#include "sim_services/Message/include/message_proto.h"
#include "trick_utils/comm/include/tc_proto.h"

/** @par Detailed Design: */
int Trick::MonteCarlo::slave_process_run() {
    int size;
    /** <ul><li> Read the length of the incoming message. */
    if (tc_read(&connection_device, (char *)&size, (int)sizeof(size)) != (int)sizeof(size) || (size = ntohl(size)) < 0) {
        if (verbosity >= ERROR) {
            message_publish(MSG_ERROR, "Monte [%s:%d] : Lost connection to Master while receiving new run.\nShutting down.\n",
                            machine_name.c_str(), slave_id) ;
        }
        slave_shutdown();
    }
    char *input = new char[size + 1];
    /** <li> Read the incoming message. */
    if (tc_read(&connection_device, input, size) != size) {
        if (verbosity >= ERROR) {
            message_publish(MSG_ERROR, "Monte [%s:%d] : Lost connection to Master while receiving new run.\nShutting down.\n",
                            machine_name.c_str(), slave_id) ;
        }
        slave_shutdown();
    }
    tc_disconnect(&connection_device);

    /**
     * <li> fork() a child process to execute the simulation. 
     * This allows the slave to monitor the child and continue running
     * if it crashes or times out.
     */
    pid_t pid = fork();
    if (pid == -1) {
        if (verbosity >= ERROR) {
            message_publish(MSG_ERROR, "Monte [%s:%d] : Unable to fork new process for run.\nShutting down.\n",
                            machine_name.c_str(), slave_id) ;
        }
        slave_shutdown();
    /** <li>Parent process: */
    } else if (pid != 0) {
        int return_value = 0 ;
        /** <li> Wait for the child to finish. */
        if (waitpid(pid, &return_value, 0) == -1) {
            /* (Alex) On the Mac this check gives a lot of false positives.  I've commented out the code for now. */
            /*
            if (verbosity >= ERROR) {
                message_publish(MSG_ERROR, "Monte [%s:%d] : Error while waiting for run to finish.\nShutting down.\n",
                                machine_name.c_str(), slave_id) ;
            }
            slave_shutdown();
            */
        }
        /** <li> Extract the exit status of the child. */
        MonteRun::ExitStatus exit_status;
        if (WIFEXITED(return_value)) {
            exit_status = (MonteRun::ExitStatus)WEXITSTATUS(return_value);
            if (exit_status == 0) {
                exit_status = MonteRun::COMPLETE;
            }
        } else {
            int signal = WTERMSIG(return_value);
            exit_status = signal == SIGALRM ? MonteRun::TIMEDOUT : MonteRun::CORED;
            if (verbosity >= ERROR) {
                message_publish(MSG_ERROR, "Monte [%s:%d] : Run killed by signal %d.\n",
                                machine_name.c_str(), slave_id, signal) ;
            }
        }
        connection_device.port = master_port;
        if (tc_connect(&connection_device) != TC_SUCCESS) {
            if (verbosity >= ERROR) {
                message_publish(MSG_ERROR, "Monte [%s:%d] : Lost connection to Master before results could be returned.\nShutting down.\n",
                                machine_name.c_str(), slave_id) ;
            }
            slave_shutdown();
        }
        if (verbosity >= ALL) {
            message_publish(MSG_INFO, "Monte [%s:%d] : Sending run exit status to master %d.\n",
                            machine_name.c_str(), slave_id, exit_status) ;

        }
        /** <li> Write the slaves id to the master. </ul> */
        int id = htonl(slave_id);
        tc_write(&connection_device, (char *)&id, (int)sizeof(id));
        /** <li> Write the child's exit status to the master. </ul> */
        return_value = htonl(exit_status);
        tc_write(&connection_device, (char *)&return_value, (int)sizeof(return_value));
        tc_disconnect(&connection_device);
        return 0;
    /** <li> Child process: */
    } else {

        input[size] = '\0';
        ip_parse(input);

        /** <ul><li> Create the run directory. */
        std::string output_dir = command_line_args_get_output_dir();
        if (access(output_dir.c_str(), F_OK) != 0) {
            if (mkdir(output_dir.c_str(), 0775) == -1) {
                exit(MonteRun::NO_PERM);
            }
        }
        char new_output[128];
        sprintf(new_output, "%s/monte_input", output_dir.c_str());
        FILE *fp = fopen(new_output, "w");
        fprintf(fp,
          "# This run can be executed in stand alone (non-Monte Carlo) mode by running\n"
          "# the S_main executable with this file specified as the input file.\n\n");
        fprintf(fp, "execfile(\"%s\")\n\n", command_line_args_get_input_file());
        fprintf(fp, "trick.mc_set_enabled(0)\n");
        fprintf(fp, "%s" , input);
        fclose(fp);
        delete [] input;

        /** <li> redirect stdout and stderr to files in the run directory */
        sprintf(new_output, "%s/stdout", output_dir.c_str());
        freopen(new_output, "w", stdout);
        sprintf(new_output, "%s/stderr", output_dir.c_str());
        freopen(new_output, "w", stderr);

        /** <li> Run the pre run jobs. */
        run_queue(&slave_pre_queue, "in slave_pre queue") ;

        /** <li> Set a timer to interrupt us after the timeout value. */
        struct sigaction default_alarm;
        default_alarm.sa_handler = SIG_DFL;
        sigaction(SIGALRM, &default_alarm, NULL);
        alarm((unsigned int)timeout);

        /**
         * <li> Return a non-zero result so the calling function (#slave)
         * will return, allowing the slave sim to complete.
         */
        return 1;
    }
    return 0;
}