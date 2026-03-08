#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>
#include "scheduler.h"
FILE *match_log;
FILE *gantt_log;
// global resources
int ball = 0;
int over = 1;
int score = 0;
int wickets = 0;
int ball_in_air = 0;
int match_over = 0; // to indicate that the match is over
int time_step = 0;

pthread_mutex_t score_mutex;
pthread_mutex_t pitch_mutex;
pthread_mutex_t log_mutex;
pthread_cond_t ball_hit_cond;

sem_t crease;

// bowler thread

void* bowler(void* arg) {

    while(1) {

        pthread_mutex_lock(&score_mutex);

        if(match_over) {
            pthread_mutex_unlock(&score_mutex);
            break;
        }

        pthread_mutex_unlock(&score_mutex);

        pthread_mutex_lock(&pitch_mutex);

        ball++;

        char *bowler = get_current_bowler();

        printf("Over %d Ball %d: %s delivers the ball\n",
               over, ball, bowler);

        pthread_mutex_lock(&log_mutex);

        time_step++;

        fprintf(gantt_log,
        "Time %d : Bowler (%s) used pitch\n",
        time_step, bowler);

        fprintf(match_log,
        "Over %d Ball %d: %s delivers the ball\n",
        over, ball, bowler);

        pthread_mutex_unlock(&log_mutex);

        if(ball >= 6) {
            printf("Over %d completed\n", over);
            over++;
            ball = 0;

            next_bowler();   // Round Robin context switch
        }

        pthread_mutex_unlock(&pitch_mutex);

        sleep(1);
    }

    pthread_exit(NULL);
}

// batsman thread

void* batsman(void* arg) {

    sem_wait(&crease);

    while(1) {
        
        pthread_mutex_lock(&score_mutex);

        if(match_over) {
            pthread_mutex_unlock(&score_mutex);
            break;
        }

        pthread_mutex_unlock(&score_mutex);

        int run = rand() % 7;

        if(run == 5) {

        printf("Batsman OUT!\n");

        pthread_mutex_lock(&log_mutex);
        fprintf(match_log, "Batsman OUT!\n");
        pthread_mutex_unlock(&log_mutex);
        pthread_mutex_lock(&pitch_mutex);
        pthread_mutex_lock(&score_mutex);
        wickets++;
        
            pthread_mutex_lock(&log_mutex);

            time_step++;

            fprintf(gantt_log,
            "Time %d : Batsman processed ball\n",
            time_step);

            pthread_mutex_unlock(&log_mutex);

        if(wickets >= 2) {
            match_over = 1;

            pthread_cond_broadcast(&ball_hit_cond);  // wake sleeping fielders
        }

        pthread_mutex_unlock(&score_mutex);

        pthread_mutex_unlock(&pitch_mutex);
        break;
    }  else {

            pthread_mutex_lock(&score_mutex);
            score += run;
            pthread_mutex_unlock(&score_mutex);

            printf("Runs scored: %d | Score: %d/%d\n", run, score, wickets);
            
            pthread_mutex_lock(&log_mutex);

            fprintf(match_log,
            "Runs scored: %d | Score: %d/%d\n",
            run, score, wickets);

            pthread_mutex_unlock(&log_mutex);

            if(run >= 4) {
                ball_in_air = 1;
                pthread_cond_broadcast(&ball_hit_cond);
            }

        }

        pthread_mutex_unlock(&pitch_mutex);

        sleep(1);
    }

    sem_post(&crease);

    pthread_exit(NULL);
}

// fielder thread

void* fielder(void* arg) {

    while(1) {

        pthread_mutex_lock(&pitch_mutex);

        while(!ball_in_air && !match_over) {
            pthread_cond_wait(&ball_hit_cond, &pitch_mutex);
        }

        if(match_over) {
            pthread_mutex_unlock(&pitch_mutex);
            break;
        }

        printf("Fielder chasing the ball...\n");

        pthread_mutex_lock(&log_mutex);
        time_step++;
        fprintf(gantt_log,
        "Time %d : Fielder fielded ball\n",
        time_step);
        pthread_mutex_unlock(&log_mutex);

        ball_in_air = 0;

        pthread_mutex_unlock(&pitch_mutex);

        sleep(1);
    }

    pthread_exit(NULL);
}

// main function

int main() {
    match_log = fopen("../results/match_log.txt", "w");

    if(match_log == NULL) {
        printf("Error opening log file\n");
        return 1;
    } // handling edge cases

    gantt_log = fopen("../results/gantt_log.txt", "w");

    if(gantt_log == NULL) {
        printf("Error opening Gantt log file\n");
        return 1;
    }

    setvbuf(match_log, NULL, _IONBF, 0);
    setvbuf(gantt_log, NULL, _IONBF, 0);

    init_scheduler();
    pthread_mutex_init(&log_mutex, NULL);
    pthread_mutex_init(&score_mutex, NULL);
    pthread_mutex_init(&pitch_mutex, NULL);

    pthread_cond_init(&ball_hit_cond, NULL);
    srand(time(NULL));

    pthread_t bowler_thread;
    pthread_t batsmen[2];
    pthread_t fielders[10];


    sem_init(&crease, 0, 2);

    pthread_create(&bowler_thread, NULL, bowler, NULL);

    for(int i = 0; i < 2; i++)
        pthread_create(&batsmen[i], NULL, batsman, NULL);

    for(int i = 0; i < 10; i++)
        pthread_create(&fielders[i], NULL, fielder, NULL);

    pthread_join(bowler_thread, NULL);

    for(int i = 0; i < 2; i++)
        pthread_join(batsmen[i], NULL);

    printf("Match Finished\n");
    printf("Final Score: %d/%d\n", score, wickets);

    pthread_mutex_destroy(&score_mutex);
    pthread_mutex_destroy(&pitch_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_cond_destroy(&ball_hit_cond);

    sem_destroy(&crease);
    fclose(gantt_log);
    fclose(match_log);
    return 0;
}
