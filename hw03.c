#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

#define N_PROCESS 10
#define STRICT_GLOBAL_RESET 1

#define READY 0
#define RUNNING 1
#define SLEEP 2
#define DONE 3

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_CYAN    "\033[36m"
#define C_YELLOW  "\033[33m"
#define C_GREEN   "\033[32m"
#define C_RED     "\033[31m"

struct pcb_struct {
    pid_t pid;
    int state;
    int left_time;
    int io_time;
    
    int arrive_time;
    int finish_time;
    int first_cpu_time;
    int wait_time;
    int first_run;
    int total_cpu;
};

struct pcb_struct pcb[N_PROCESS];
int current_idx = -1;
int process_count = N_PROCESS;
int time_quantum = 3;

int sys_time = 0;
int idle_time = 0;
int ctx_switch = 0;

void func_parent();
void func_child(int id);
void isr_scheduler(int sig);
void isr_io(int sig);
void isr_child_exit(int sig);
void child_task(int sig);

int child_burst = 0;

void print_line() {
    printf("----------------------------------------------------------------------------------\n");
}

int main() {
    srand(time(NULL));
    int i;
    pid_t pid;

    printf("\n[Config] Enter Time Quantum (Recommendation: 1~10): ");
    scanf("%d", &time_quantum);
    if (time_quantum <= 0) time_quantum = 3;

    printf("\n");
    print_line();
    printf("  %-10s  %-6s  %-35s  %-20s\n", 
           "TYPE", "PID", "EVENT", "STATUS/INFO");
    print_line();

    for (i = 0; i < N_PROCESS; i++) {
        pid = fork();
        if (pid == 0) {
            func_child(i);
            exit(0);
        } else if (pid > 0) {
            pcb[i].pid = pid;
            pcb[i].state = READY;
            pcb[i].left_time = time_quantum;
            pcb[i].io_time = 0;
            
            pcb[i].arrive_time = 0;
            pcb[i].wait_time = 0;
            pcb[i].total_cpu = 0;
            pcb[i].first_run = 1;
            pcb[i].first_cpu_time = -1;
            
            usleep(10000); 
        } else {
            perror("fork error");
            exit(1);
        }
    }

    func_parent();
    return 0;
}

void func_parent() {
    signal(SIGALRM, isr_scheduler);
    signal(SIGUSR1, isr_io);
    signal(SIGCHLD, isr_child_exit);

    sleep(1);

    print_line();
    printf("  " C_YELLOW "%-10s" C_RESET "  %-6s  " C_YELLOW "%-35s" C_RESET "  Quantum: %d\n", 
           "KERNEL", "-", "SYSTEM SIMULATION START", time_quantum);
    print_line();

    alarm(1);

    while (process_count > 0) {
        pause();
    }

    print_line();
    printf("  " C_YELLOW "%-10s" C_RESET "  %-6s  " C_YELLOW "%-35s" C_RESET "\n", 
           "KERNEL", "-", "SIMULATION FINISHED");
    print_line();
    
    printf("  %-8s  %-5s  %-10s  %-12s  %-12s\n", 
           "RESULT", "PID", "Wait", "Turnaround", "Response");
    printf("  --------  -----  ----------  ------------  ------------\n");

    double avg_wait = 0;
    double avg_turnaround = 0;
    double avg_response = 0;
    int i;

    for (i = 0; i < N_PROCESS; i++) {
        int turnaround = pcb[i].finish_time - pcb[i].arrive_time;
        int response = 0;
        if (pcb[i].first_cpu_time != -1) {
            response = pcb[i].first_cpu_time - pcb[i].arrive_time;
        }

        printf("  RESULT    %5d  %10d  %12d  %12d\n", 
            pcb[i].pid, pcb[i].wait_time, turnaround, response);

        avg_wait += pcb[i].wait_time;
        avg_turnaround += turnaround;
        avg_response += response;
    }

    double util = ((double)(sys_time - idle_time) / sys_time) * 100.0;
    double throughput = (double)N_PROCESS / sys_time;

    print_line();
    printf(C_BOLD "  [ System Performance Summary ]" C_RESET "\n");
    printf("  1. Avg Waiting Time      : %.2f ticks\n", avg_wait / N_PROCESS);
    printf("  2. Avg Turnaround Time   : %.2f ticks\n", avg_turnaround / N_PROCESS);
    printf("  3. Avg Response Time     : %.2f ticks\n", avg_response / N_PROCESS);
    printf("  4. CPU Utilization       : %.2f %% (Busy: %d / Total: %d)\n", 
           util, sys_time - idle_time, sys_time);
    printf("  5. Throughput            : %.4f proc/tick\n", throughput);
    printf("  6. Context Switches      : %d\n", ctx_switch);
    print_line();
}

void isr_scheduler(int sig) {
    sys_time++;
    int i;

    for (i = 0; i < N_PROCESS; i++) {
        if (pcb[i].state == SLEEP) {
            pcb[i].io_time--;
            if (pcb[i].io_time <= 0) {
                pcb[i].state = READY;
                printf("  " C_GREEN "%-10s" C_RESET "  %5d   " C_GREEN "%-35s" C_RESET "  Ready Queue\n", 
                       "KERNEL", pcb[i].pid, "Wake up from I/O");
            }
        } else if (pcb[i].state == READY) {
            pcb[i].wait_time++;
        }
    }

    if (current_idx != -1 && pcb[current_idx].state == RUNNING) {
        pcb[current_idx].left_time--;
        pcb[current_idx].total_cpu++;

        if (pcb[current_idx].left_time <= 0) {
            pcb[current_idx].state = READY;
            printf("  " C_YELLOW "%-10s" C_RESET "  %5d   %-35s  Q Expired -> Ready\n", 
                   "KERNEL", pcb[current_idx].pid, "Time Quantum Expired");
            print_line(); 
            current_idx = -1;
            ctx_switch++;
        }
    }

    int reset_flag = 1;
    for (i = 0; i < N_PROCESS; i++) {
        if (pcb[i].state != DONE) {
            #if STRICT_GLOBAL_RESET
            if (pcb[i].left_time > 0) {
                reset_flag = 0;
                break;
            }
            #else
            if ((pcb[i].state == READY || pcb[i].state == RUNNING) && pcb[i].left_time > 0) {
                reset_flag = 0;
                break;
            }
            #endif
        }
    }

    if (reset_flag == 1 && process_count > 0) {
        print_line();
        printf("  " C_RED "%-10s" C_RESET "  %-6s  " C_RED "%-35s" C_RESET "  Refill Quantums\n", 
               "KERNEL", "ALL", "*** GLOBAL RESET ***");
        print_line();
        for (i = 0; i < N_PROCESS; i++) {
            if (pcb[i].state != DONE) {
                pcb[i].left_time = time_quantum;
            }
        }
    }

    if (current_idx == -1) {
        static int search_idx = -1;
        int k;
        int found = 0;

        for (k = 0; k < N_PROCESS; k++) {
            int idx = (search_idx + 1 + k) % N_PROCESS;
            if (pcb[idx].state == READY && pcb[idx].left_time > 0) {
                current_idx = idx;
                pcb[current_idx].state = RUNNING;
                search_idx = current_idx;
                found = 1;

                if (pcb[current_idx].first_run == 1) {
                    pcb[current_idx].first_cpu_time = sys_time - 1;
                    pcb[current_idx].first_run = 0;
                }
                break;
            }
        }

        if (found == 0 && process_count > 0) {
            idle_time++;
        }
    }

    if (current_idx != -1) {
        kill(pcb[current_idx].pid, SIGUSR1);
    }

    if (process_count > 0) alarm(1);
}

void isr_io(int sig) {
    if (current_idx != -1) {
        pcb[current_idx].state = SLEEP;
        pcb[current_idx].io_time = (rand() % 5) + 1;
        
        printf("  " C_YELLOW "%-10s" C_RESET "  %5d   %-35s  Sleep: %d ticks\n", 
               "KERNEL", pcb[current_idx].pid, "I/O Requested", pcb[current_idx].io_time);
        print_line(); 
        
        current_idx = -1;
        ctx_switch++;
    }
}

void isr_child_exit(int sig) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int i;
        for (i = 0; i < N_PROCESS; i++) {
            if (pcb[i].pid == pid) {
                pcb[i].state = DONE;
                pcb[i].finish_time = sys_time;
                process_count--;
                
                if (current_idx == i) {
                    current_idx = -1;
                    ctx_switch++;
                }
                printf("  " C_RED "%-10s" C_RESET "  %5d   " C_RED "%-35s" C_RESET "  State: DONE\n", 
                       "KERNEL", pid, "Process Terminated");
                print_line();
            }
        }
    }
}

void func_child(int id) {
    srand(time(NULL) ^ getpid());
    signal(SIGUSR1, child_task);

    child_burst = (rand() % 10) + 1;
    
    char bar[12];
    memset(bar, ' ', 10);
    bar[10] = '\0';

    int k;
    for(k=0; k<child_burst && k<10; k++) bar[k] = '*';

    printf("  " C_CYAN "%-10s" C_RESET "  %5d   Created (Burst: %-2d)                 [%s]\n", 
           "CHILD", getpid(), child_burst, bar);

    while (1) {
        pause();
    }
}

void child_task(int sig) {
    if (child_burst <= 0) {
        child_burst = (rand() % 5) + 1;
        printf("  " C_CYAN "%-10s" C_RESET "  %5d   I/O Done -> New Burst: %-2d\n", 
               "CHILD", getpid(), child_burst);
    }

    child_burst--;
    
    char bar[12];
    memset(bar, ' ', 10);
    bar[10] = '\0';

    int k;
    for(k=0; k<child_burst && k<10; k++) bar[k] = '*';

    printf("  " C_CYAN "%-10s" C_RESET "  %5d   " C_CYAN "%-35s" C_RESET "  Burst: [%s]\n", 
           "CHILD", getpid(), "Running...", bar);

    if (child_burst <= 0) {
        int action = rand() % 2; 
        if (action == 0) {
            exit(0); 
        } else {
            kill(getppid(), SIGUSR1);
        }
    }
}
