
#include "scheduler.h"

#define NUM_BOWLERS 3

char* bowlers[NUM_BOWLERS] = {
    "Bumrah",
    "Shami",
    "Siraj"
};

int current_bowler = 0;

void init_scheduler() {
    current_bowler = 0;
}

char* get_current_bowler() {
    return bowlers[current_bowler];
}

void next_bowler() {
    current_bowler = (current_bowler + 1) % NUM_BOWLERS;
}