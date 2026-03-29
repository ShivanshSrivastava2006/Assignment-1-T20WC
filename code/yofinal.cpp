#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <semaphore.h>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>
#include <map>
using namespace std;

static const int TOTAL_OVERS = 20;
static const int BALLS_PER_OVER = 6;
static const int MAX_PLAYERS = 11;
static const int TEAM_COUNT = 2;
static const int AVAILABLE_TEAMS = 4;

enum CountryId { COUNTRY_INDIA = 0, COUNTRY_PAKISTAN = 1, COUNTRY_ENGLAND = 2, COUNTRY_AUSTRALIA = 3 };

enum PlayerState { WAITING, BATTING, BOWLING, FIELDING, OUT_STATE };
enum DeliveryType { DELIVERY_FAIR, DELIVERY_WIDE, DELIVERY_BOWLED, DELIVERY_NO_BALL };
enum DismissalType { DISMISS_NONE, DISMISS_BOWLED, DISMISS_CAUGHT, DISMISS_RUN_OUT };

struct BatsmanInfo {
    int runs_scored;
    int balls_faced;
    double avg;
    long long wait_ms;
    int batting_position;
};

struct BowlerInfo {
    int wickets;
    int runs_conceded;
    int legal_balls;
    int overs_bowled;
    double eco;
};

struct Player {
    int id;
    char name[32];
    int team;
    PlayerState state;
    bool pending_activation;
    bool has_active_slot;
    int held_end;
    pthread_t thread;
    BatsmanInfo bat;
    BowlerInfo bowl;
};

struct SyncPrims {
    pthread_mutex_t mutex_umpire;
    pthread_mutex_t mutex_score;
    pthread_mutex_t mutex_catch;
    pthread_mutex_t mutex_gantt;
    pthread_mutex_t mutex_end_A;
    pthread_mutex_t mutex_end_B;
    pthread_cond_t cond_umpire_wake;
    pthread_cond_t cond_batsman_wake;
    pthread_cond_t cond_bowler_wake;
    pthread_cond_t cond_fielders_wake;
    sem_t active_batsmen_slots;
};

struct RunGraphState {
    bool active;
    int total_legs;
    int current_leg;
    int serial;
    int player_ids[2];
    int player_end[2];
    bool request_edge[2][2];
    bool allocation_edge[2][2];
    bool leg_ready[2];
    bool leg_complete[2];
    bool deadlock_detected;
    bool victim_released;
    int deadlock_victim_id;
    int deadlock_survivor_id;
};

struct MatchState {
    int innings;
    int current_over;
    int legal_balls_in_over;
    int scores[TEAM_COUNT];
    int wickets[TEAM_COUNT];
    int target;

    int active_bowler_id;
    int striker_id;
    int non_striker_id;
    int next_bat_idx;
    int batting_team_id;
    int bowling_team_id;
    int ball_serial;

    bool innings_complete;
    bool bowler_turn;
    bool striker_turn;
    bool ball_resolved;
    bool free_hit_next_ball;
    bool free_hit_active;
    bool catch_window_open;

    DeliveryType delivery_type;
    DismissalType dismissal_type;
    int runs_this_ball;
    int batsman_runs_this_ball;
    int runs_attempted_this_ball;
    int dismissed_batsman_id;
    int runout_survivor_id;
    bool wicket_this_ball;
    bool extra_this_ball;
    bool legal_ball_completed;
    bool catch_taken;
    int catcher_id;
    char play_result_log[160];
};

struct OverReport {
    int innings;
    int over_number;
    int bowling_team_id;
    int bowler_id;
    int runs;
    int wickets;
    bool high_intensity;
};

struct TimelineEntry {
    int innings;
    string marker;
    string bowler;
    string striker;
    string non_striker;
    string result;
};

struct GanttEvent {
    int innings;
    long long time_us;
    string actor;
};

struct PlayerTemplate {
    const char* name;
    double bat_avg;
    double bowl_eco;
};

struct BallRenderInfo {
    string result_token;
    string remark;
    const char* result_style;
};

struct CliConfig {
    bool use_sjf;
    bool team_explicit[TEAM_COUNT];
    bool strategy_explicit;
};

struct SummaryPlayerRef {
    const Player* player;
    int team_id;
};

SyncPrims S;
MatchState ms;
RunGraphState run_graph;

Player teamA[MAX_PLAYERS];
Player teamB[MAX_PLAYERS];
Player* batting_team = nullptr;
Player* bowling_team = nullptr;

int batting_order[MAX_PLAYERS];
int bowling_candidates[5];
int bowler_rotation[TOTAL_OVERS];
int innings_batting_team_id[2];
int innings_bowling_team_id[2];
int selected_country_ids[TEAM_COUNT];
vector<OverReport> innings_reports[2];
vector<TimelineEntry> gantt_entries;
vector<GanttEvent> gantt_events;
string current_actor = "Idle";
int innings_batting_order[2][MAX_PLAYERS];
bool innings_use_sjf[2];
bool current_innings_use_sjf = false;
chrono::steady_clock::time_point gantt_start_clock;
int current_gantt_innings = 0;
unsigned match_seed = 0;

const char* team_labels[AVAILABLE_TEAMS] = {"India", "Pakistan", "England", "Australia"};
static bool ui_stdout_is_tty = false;
static bool ui_stdin_is_tty = false;
static bool ui_color_enabled = false;
static const int UI_RULE_WIDTH = 88;
static const int UI_COL_BATTER = 17;
static const int UI_COL_REMARK = 24;
static const char* UI_RESET = "\033[0m";
static const char* UI_BOLD_CYAN = "\033[1;36m";
static const char* UI_BOLD_GREEN = "\033[1;32m";
static const char* UI_BOLD_YELLOW = "\033[1;33m";
static const char* UI_BOLD_RED = "\033[1;31m";
static const char* UI_BOLD_WHITE = "\033[1;37m";
static const char* UI_DIM = "\033[2m";
static const char* UI_BLUE = "\033[0;34m";

const PlayerTemplate team_templates[AVAILABLE_TEAMS][MAX_PLAYERS] = {
    {
        {"Rohit Sharma", 50.05, 9.00},
        {"Virat Kohli", 28.50, 8.80},
        {"Shubman Gill", 35.03, 9.20},
        {"Suryakumar Yadav", 36.35, 8.80},
        {"Hardik Pandya", 29.15, 8.26},
        {"Rinku Singh", 19.00, 8.60},
        {"Axar Patel", 12.42, 7.32},
        {"Ravindra Jadeja", 15.45, 7.13},
        {"Jasprit Bumrah", 2.00, 6.77},
        {"Arshdeep Singh", 5.93, 7.95},
        {"Kuldeep Yadav", 9.40, 6.87},
    },
    {
        {"Babar Azam", 38.94, 9.20},
        {"Mohammad Rizwan", 47.00, 9.00},
        {"Fakhar Zaman", 23.98, 9.20},
        {"Salman Agha", 22.90, 7.60},
        {"Iftikhar Ahmed", 24.50, 7.80},
        {"Shadab Khan", 19.40, 7.40},
        {"Mohammad Nawaz", 17.51, 7.19},
        {"Shaheen Afridi", 10.00, 7.75},
        {"Haris Rauf", 8.41, 8.38},
        {"Naseem Shah", 6.88, 8.13},
        {"Abrar Ahmed", 1.33, 6.67},
    },
    {
        {"Jos Buttler", 36.50, 9.20},
        {"Phil Salt", 31.00, 9.20},
        {"Harry Brook", 28.50, 9.00},
        {"Dawid Malan", 36.38, 9.10},
        {"Liam Livingstone", 27.50, 8.05},
        {"Moeen Ali", 21.50, 7.85},
        {"Sam Curran", 24.50, 8.35},
        {"Jofra Archer", 12.50, 7.65},
        {"Adil Rashid", 10.50, 7.30},
        {"Mark Wood", 8.50, 8.20},
        {"Chris Woakes", 14.50, 8.10},
    },
    {
        {"David Warner", 50.40, 9.20},
        {"Travis Head", 40.00, 8.90},
        {"Josh Inglis", 35.50, 9.10},
        {"Mitchell Marsh", 28.50, 8.15},
        {"Glenn Maxwell", 19.50, 7.65},
        {"Marcus Stoinis", 20.80, 8.45},
        {"Tim David", 15.00, 9.20},
        {"Pat Cummins", 10.50, 7.65},
        {"Mitchell Starc", 8.80, 8.10},
        {"Adam Zampa", 6.50, 7.11},
        {"Josh Hazlewood", 3.00, 7.55},
    }
};
chrono::steady_clock::time_point innings_start_clock;

double randf() {
    return static_cast<double>(rand()) / (static_cast<double>(RAND_MAX) + 1.0);
}

long long gantt_elapsed_us() {
    return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - gantt_start_clock).count();
}

void record_gantt_event(const string& actor_name) {
    if (actor_name.empty()) return;

    long long t = gantt_elapsed_us();

    pthread_mutex_lock(&S.mutex_gantt);

    
    if (!gantt_events.empty() &&
        gantt_events.back().innings == current_gantt_innings &&
        t <= gantt_events.back().time_us) {
        t = gantt_events.back().time_us + 1;
    }

    current_actor = actor_name;
    gantt_events.push_back({current_gantt_innings, t, actor_name});

    pthread_mutex_unlock(&S.mutex_gantt);
}

int random_delay_us(int min_us, int max_us) {
    return min_us + (rand() % (max_us - min_us + 1));
}

long long elapsed_ms_since(const chrono::steady_clock::time_point& start) {
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
}

double clamp_double(double value, double lo, double hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

double batting_skill_bias(double batting_avg) {
    return clamp_double((batting_avg - 28.0) / 22.0, -1.0, 1.0);
}

double bowling_skill_bias(double bowling_eco) {
    return clamp_double((7.8 - bowling_eco) / 1.6, -1.0, 1.0);
}

string lowercase_copy(const char* input) {
    string out = input ? string(input) : string();
    transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return out;
}

int sample_weighted_index(const double* weights, int count) {
    double total = 0.0;
    for (int i = 0; i < count; i++) total += weights[i];

    double pick = randf() * total;
    double prefix = 0.0;
    for (int i = 0; i < count; i++) {
        prefix += weights[i];
        if (pick <= prefix) return i;
    }
    return count - 1;
}

int choose_runs_attempted(double batting_avg, double bowling_eco) {
    const int outcomes[6] = {0, 1, 2, 3, 4, 6};
    double weights[6] = {0.30, 0.31, 0.15, 0.03, 0.15, 0.06};

    double bat_bias = batting_skill_bias(batting_avg);
    double bowl_bias = bowling_skill_bias(bowling_eco);
    (void)bowling_eco;


    weights[0] *= 1.0 - 0.22 * bat_bias + 0.18 * bowl_bias;
    weights[1] *= 1.0 + 0.03 * bat_bias - 0.02 * bowl_bias;
    weights[2] *= 1.0 + 0.08 * bat_bias - 0.05 * bowl_bias;
    weights[3] *= 1.0 + 0.02 * bat_bias - 0.03 * bowl_bias;
    weights[4] *= 1.0 + 0.22 * bat_bias - 0.16 * bowl_bias;
    weights[5] *= 1.0 + 0.18 * bat_bias - 0.14 * bowl_bias;
    

    for (double& weight : weights) weight = max(weight, 0.01);
    return outcomes[sample_weighted_index(weights, 6)];
}

bool choose_lofted_boundary(int runs_attempted, double batting_avg, double bowling_eco) {
    if (runs_attempted < 4) return false;

    double bat_bias = batting_skill_bias(batting_avg);
    double bowl_bias = bowling_skill_bias(bowling_eco);
    double pressure_bias = clamp_double(bowl_bias - bat_bias, -1.0, 1.0);
    double lofted_probability = clamp_double(0.16 + 0.02 * bat_bias + 0.04 * pressure_bias, 0.10, 0.24);
    
    return randf() < lofted_probability;
}

double wicket_matchup_bias(double batting_avg, double bowling_eco) {
    double bat_bias = batting_skill_bias(batting_avg);
    double bowl_bias = bowling_skill_bias(bowling_eco);


    return clamp_double(0.70 * bowl_bias - 0.90 * bat_bias, -1.0, 1.0);
    
}

void compute_delivery_probabilities(double batting_avg, double bowling_eco,
                                    double& wide_probability, double& no_ball_probability,
                                    double& wicket_probability) {
    double bowl_bias = bowling_skill_bias(bowling_eco);
    double matchup_bias = wicket_matchup_bias(batting_avg, bowling_eco);
    wide_probability = clamp_double(0.03 - 0.006 * bowl_bias, 0.015, 0.045);
    no_ball_probability = clamp_double(0.02 - 0.005 * bowl_bias, 0.01, 0.03);


    wicket_probability = clamp_double(0.020 + 0.010 * matchup_bias + 0.003 * bowl_bias, 0.011, 0.036);
}

bool choose_catch_success(double batting_avg, double bowling_eco) {
    double bat_bias = batting_skill_bias(batting_avg);
    double bowl_bias = bowling_skill_bias(bowling_eco);
    double catch_probability = clamp_double(0.07 + 0.014 * bowl_bias - 0.015 * bat_bias, 0.045, 0.11);
    return randf() < catch_probability;
}

const char* intensity_label(int runs) {
    if (runs >= 15) return "HIGH";
    if (runs >= 9) return "GOOD";
    if (runs >= 5) return "STEADY";
    return "LOW";
}

string format_ball_marker(int over_number, int legal_balls_completed, bool extra_ball) {
    char out[16];
    int shown_ball = extra_ball ? (legal_balls_completed + 1) : legal_balls_completed;
    snprintf(out, sizeof(out), "%d.%d%s", max(0, over_number - 1), shown_ball, extra_ball ? "*" : "");
    return string(out);
}

string styled(const string& text, const char* code) {
    if (!ui_color_enabled || code == nullptr || code[0] == '\0') return text;
    return string(code) + text + UI_RESET;
}

void print_ui_rule(char fill = '=') {
    for (int i = 0; i < UI_RULE_WIDTH; i++) putchar(fill);
    putchar('\n');
}

string format_score_text(int score, int wickets) {
    char out[16];
    snprintf(out, sizeof(out), "%d/%d", score, wickets);
    return string(out);
}

void print_ui_padded(const string& text, int width, bool right_align = false, const char* style_code = nullptr) {
    string clipped = text.substr(0, static_cast<size_t>(max(width, 0)));
    int padding = max(0, width - static_cast<int>(clipped.size()));
    if (right_align) {
        for (int i = 0; i < padding; i++) putchar(' ');
        fputs(styled(clipped, style_code).c_str(), stdout);
    } else {
        fputs(styled(clipped, style_code).c_str(), stdout);
        for (int i = 0; i < padding; i++) putchar(' ');
    }
}

const char* ball_result_style(const string& token) {
    if (token == "W") return UI_BOLD_RED;
    if (token == "Wd" || token.rfind("Nb", 0) == 0) return UI_BOLD_YELLOW;
    if (token == "4" || token == "6") return UI_BOLD_GREEN;
    if (token == "0") return UI_DIM;
    return UI_BOLD_CYAN;
}

string base_run_remark(int runs) {
    switch (runs) {
        case 0: return "Dot ball";
        case 1: return "Quick single";
        case 2: return "Two runs";
        case 3: return "Three taken";
        case 4: return "Boundary";
        case 6: return "Maximum";
        default: return "Runs completed";
    }
}

BallRenderInfo describe_ball_for_display(bool free_hit_active) {
    BallRenderInfo info;
    info.result_style = UI_BOLD_WHITE;

    if (ms.wicket_this_ball) {
        info.result_token = "W";
        if (ms.dismissal_type == DISMISS_CAUGHT && ms.catcher_id >= 0) {
            info.remark = string("Caught by ") + bowling_team[ms.catcher_id].name;
        } else if (ms.dismissal_type == DISMISS_BOWLED) {
            info.remark = "Bowled/LBW";
        } else if (ms.dismissal_type == DISMISS_RUN_OUT) {
            info.remark = (ms.delivery_type == DELIVERY_NO_BALL)
                ? "Run out on no ball | Deadlock detected"
                : "Run out | Deadlock detected";
        } else {
            info.remark = "Wicket";
        }
    } else if (ms.delivery_type == DELIVERY_WIDE) {
        info.result_token = "Wd";
        info.remark = "Wide";
    } else if (ms.delivery_type == DELIVERY_NO_BALL) {
        info.result_token = (ms.batsman_runs_this_ball > 0)
            ? string("Nb+") + to_string(ms.batsman_runs_this_ball)
            : string("Nb");
        info.remark = (ms.batsman_runs_this_ball > 0)
            ? string("No ball + ") + to_string(ms.batsman_runs_this_ball)
            : string("No ball");
    } else {
        info.result_token = to_string(ms.batsman_runs_this_ball);
        info.remark = base_run_remark(ms.batsman_runs_this_ball);
    }

    if (free_hit_active) {
        if (!info.remark.empty()) info.remark += " | Free hit";
        else info.remark = "Free hit";
    }

    info.result_style = ball_result_style(info.result_token);
    return info;
}

void print_divider() {
    printf("========================================================================\n");
}

void print_match_banner(bool use_sjf) {
    print_ui_rule('=');
    printf(" %s\n", styled("T20 CRICKET SIMULATION", UI_BOLD_CYAN).c_str());
    printf(" %s %s vs %s\n", styled("Match ", UI_BOLD_WHITE).c_str(),
           team_labels[selected_country_ids[0]], team_labels[selected_country_ids[1]]);
    printf(" %s %s batting order\n", styled("Mode  ", UI_BOLD_WHITE).c_str(),
           use_sjf ? "SJF" : "FCFS");
    printf(" %s %u\n", styled("Seed  ", UI_BOLD_WHITE).c_str(), match_seed);
    printf(" %s %d overs per innings\n", styled("Format", UI_BOLD_WHITE).c_str(), TOTAL_OVERS);
    print_ui_rule('=');
}

void print_toss_banner(int toss_winner, int toss_choice) {
    printf(" %s %s won the toss and chose to %s first.\n",
           styled("Toss  ", UI_BOLD_YELLOW).c_str(),
           team_labels[selected_country_ids[toss_winner]],
           toss_choice == 0 ? "bat" : "bowl");
}

void print_innings_header(const char* title, int target = -1) {
    printf("\n");
    print_ui_rule('-');
    if (target >= 0) printf(" %s  |  %s %d\n", styled(title, UI_BOLD_CYAN).c_str(),
                            styled("Target", UI_BOLD_YELLOW).c_str(), target);
    else printf(" %s\n", styled(title, UI_BOLD_CYAN).c_str());
    print_ui_rule('-');
}

void print_ball_table_header() {
    printf(" %5s | %-17s | %-17s | %5s | %-6s | %-24s\n",
           "Ball", "Striker", "Non-Striker", "Score", "Result", "Remark");
    printf("-------+-------------------+-------------------+-------+--------+--------------------------\n");
}

void print_over_header(int over_number, const char* bowler_name, int score, int wickets, bool free_hit_pending) {
    printf("\n");
    print_ui_rule('-');
    printf(" %s %02d  |  %s %-18.18s  |  %s %3d/%d",
           styled("OVER", UI_BOLD_CYAN).c_str(),
           over_number,
           styled("Bowler", UI_BOLD_WHITE).c_str(),
           bowler_name,
           styled("Start", UI_BOLD_WHITE).c_str(),
           score, wickets);
    if (ms.innings == 1) {
        int target_score = ms.target + 1;
        int runs_needed = max(0, target_score - score);
        int balls_left = (TOTAL_OVERS - (over_number - 1)) * BALLS_PER_OVER;
        printf("  |  %s %d off %d", styled("Chase", UI_BOLD_YELLOW).c_str(), runs_needed, balls_left);
    }
    if (free_hit_pending) printf("  |  %s", styled("Next Ball FH", UI_BOLD_YELLOW).c_str());
    printf("\n");
    print_ball_table_header();
}

void print_ball_event(const string& marker, const char* striker_name, const char* non_striker_name,
                      int score, int wickets, const BallRenderInfo& render_info) {
    printf(" ");
    print_ui_padded(marker, 5);
    printf(" | ");
    print_ui_padded(striker_name ? string(striker_name) : string(), UI_COL_BATTER);
    printf(" | ");
    print_ui_padded(non_striker_name ? string(non_striker_name) : string(), UI_COL_BATTER);
    printf(" | ");
    print_ui_padded(format_score_text(score, wickets), 5, true);
    printf(" | ");
    print_ui_padded(render_info.result_token, 6, false, render_info.result_style);
    printf(" | ");
    print_ui_padded(render_info.remark, UI_COL_REMARK);
    printf("\n");
}

void print_over_summary(int over_number, int runs, int wickets, int score, int total_wickets) {
    double run_rate = (over_number > 0) ? (static_cast<double>(score) / static_cast<double>(over_number)) : 0.0;
    printf(" %s O%02d -> Runs: %2d | Wkts: %d | RR: %4.2f | Intensity: %-6s | Total: %3d/%d\n",
           styled("Summary", UI_BLUE).c_str(),
           over_number, runs, wickets, run_rate, intensity_label(runs), score, total_wickets);
}

void clear_run_graph() {
    memset(&run_graph, 0, sizeof(run_graph));
    run_graph.deadlock_victim_id = -1;
    run_graph.deadlock_survivor_id = -1;
}

int run_graph_player_index(int player_id) {
    for (int i = 0; i < 2; i++) {
        if (run_graph.player_ids[i] == player_id) return i;
    }
    return -1;
}

pthread_mutex_t* pitch_end_mutex(int end) {
    return (end == 0) ? &S.mutex_end_A : &S.mutex_end_B;
}

bool dfs_cycle_visit(int node, bool adj[4][4], int color[4]) {
    color[node] = 1;
    for (int next = 0; next < 4; next++) {
        if (!adj[node][next]) continue;
        if (color[next] == 1) return true;
        if (color[next] == 0 && dfs_cycle_visit(next, adj, color)) return true;
    }
    color[node] = 2;
    return false;
}

bool run_graph_has_cycle() {
    bool adj[4][4];
    memset(adj, 0, sizeof(adj));

    for (int resource = 0; resource < 2; resource++) {
        for (int process = 0; process < 2; process++) {
            if (run_graph.allocation_edge[resource][process]) adj[resource][2 + process] = true;
            if (run_graph.request_edge[process][resource]) adj[2 + process][resource] = true;
        }
    }

    int color[4] = {0, 0, 0, 0};
    for (int node = 0; node < 4; node++) {
        if (color[node] == 0 && dfs_cycle_visit(node, adj, color)) return true;
    }
    return false;
}

void reset_run_leg_state_locked() {
    for (int resource = 0; resource < 2; resource++) {
        for (int process = 0; process < 2; process++) {
            run_graph.request_edge[process][resource] = false;
        }
    }
    run_graph.leg_ready[0] = false;
    run_graph.leg_ready[1] = false;
    run_graph.leg_complete[0] = false;
    run_graph.leg_complete[1] = false;
}

void start_run_graph(int runs_attempted) {
    clear_run_graph();
    run_graph.active = true;
    run_graph.serial = ms.ball_serial;
    run_graph.total_legs = runs_attempted;
    run_graph.current_leg = 0;
    run_graph.player_ids[0] = ms.striker_id;
    run_graph.player_ids[1] = ms.non_striker_id;
    run_graph.player_end[0] = 0;
    run_graph.player_end[1] = 1;
    reset_run_leg_state_locked();
}

void sync_batsmen_from_run_graph() {
    if (run_graph.player_end[0] == 0) {
        ms.striker_id = run_graph.player_ids[0];
        ms.non_striker_id = run_graph.player_ids[1];
    } else {
        ms.striker_id = run_graph.player_ids[1];
        ms.non_striker_id = run_graph.player_ids[0];
    }
}

void finalize_safe_run_locked() {
    sync_batsmen_from_run_graph();
    ms.runs_this_ball = ms.runs_attempted_this_ball + ((ms.delivery_type == DELIVERY_NO_BALL) ? 1 : 0);
    ms.batsman_runs_this_ball = ms.runs_attempted_this_ball;
    ms.ball_resolved = true;
    ms.wicket_this_ball = false;
    ms.dismissal_type = DISMISS_NONE;
    if (ms.delivery_type == DELIVERY_NO_BALL) {
        snprintf(ms.play_result_log, sizeof(ms.play_result_log), "No ball + %d runs completed", ms.runs_attempted_this_ball);
    } else {
        snprintf(ms.play_result_log, sizeof(ms.play_result_log), "%d runs completed", ms.runs_attempted_this_ball);
    }
    run_graph.active = false;
    pthread_cond_broadcast(&S.cond_batsman_wake);
    pthread_cond_signal(&S.cond_umpire_wake);
}

void advance_run_leg_locked() {
    run_graph.current_leg++;
    reset_run_leg_state_locked();
    pthread_cond_broadcast(&S.cond_batsman_wake);
}

void declare_run_out_deadlock_locked() {
    run_graph.deadlock_detected = true;
    run_graph.deadlock_victim_id = run_graph.player_ids[1];
    run_graph.deadlock_survivor_id = run_graph.player_ids[0];
    record_gantt_event(
    string("RUNOUT: ") + batting_team[run_graph.deadlock_victim_id].name
    );
    ms.runs_this_ball = (ms.delivery_type == DELIVERY_NO_BALL) ? 1 : 0;
    ms.batsman_runs_this_ball = 0;
    ms.wicket_this_ball = true;
    ms.dismissal_type = DISMISS_RUN_OUT;
    ms.dismissed_batsman_id = run_graph.deadlock_victim_id;
    ms.runout_survivor_id = run_graph.deadlock_survivor_id;
    snprintf(ms.play_result_log, sizeof(ms.play_result_log), "RUN OUT! DFS deadlock detected");
    pthread_cond_broadcast(&S.cond_batsman_wake);
}

void maybe_finish_running_ball_locked() {
    if (!run_graph.active || run_graph.deadlock_detected) return;
    if (!run_graph.leg_complete[0] || !run_graph.leg_complete[1]) return;

    if (run_graph.current_leg + 1 >= run_graph.total_legs) finalize_safe_run_locked();
    else advance_run_leg_locked();
}

void activate_batsman_locked(int order_index) {
    if (order_index >= MAX_PLAYERS) return;
    int player_id = batting_order[order_index];
    Player* p = &batting_team[player_id];
    p->pending_activation = true;
    p->bat.batting_position = order_index + 1;
    pthread_cond_broadcast(&S.cond_batsman_wake);
    while (!p->has_active_slot && !ms.innings_complete) {
        pthread_cond_wait(&S.cond_batsman_wake, &S.mutex_umpire);
    }
}

void build_batting_schedule(bool use_sjf) {
    for (int i = 0; i < MAX_PLAYERS; i++) batting_order[i] = i;
    if (!use_sjf) return;

    sort(batting_order, batting_order + MAX_PLAYERS, [](int lhs, int rhs) {
        return batting_team[lhs].bat.avg < batting_team[rhs].bat.avg;
    });
}

void build_bowler_schedule() {
    vector<int> order;
    for (int i = 0; i < MAX_PLAYERS; i++) order.push_back(i);

    sort(order.begin(), order.end(), [](int lhs, int rhs) {
        if (bowling_team[lhs].bowl.eco == bowling_team[rhs].bowl.eco) return lhs < rhs;
        return bowling_team[lhs].bowl.eco < bowling_team[rhs].bowl.eco;
    });

    for (int i = 0; i < 5; i++) bowling_candidates[i] = order[i];
    for (int i = 0; i < TOTAL_OVERS; i++) bowler_rotation[i] = -1;
}

int choose_round_robin_bowler(int over_idx, int previous_bowler) {
    for (int offset = 0; offset < 5; offset++) {
        int candidate_id = bowling_candidates[(over_idx + offset) % 5];
        if (bowling_team[candidate_id].bowl.overs_bowled >= 4) continue;
        if (candidate_id == previous_bowler) continue;
        return candidate_id;
    }

    for (int i = 0; i < 5; i++) {
        int candidate_id = bowling_candidates[i];
        if (bowling_team[candidate_id].bowl.overs_bowled < 4) return candidate_id;
    }
    return bowling_candidates[0];
}

int choose_death_specialist(int previous_bowler) {
    int best_id = -1;
    for (int i = 0; i < 5; i++) {
        int candidate_id = bowling_candidates[i];
        if (bowling_team[candidate_id].bowl.overs_bowled >= 4) continue;
        if (candidate_id == previous_bowler) continue;
        if (best_id == -1 || bowling_team[candidate_id].bowl.eco < bowling_team[best_id].bowl.eco) best_id = candidate_id;
    }
    return best_id;
}

int select_bowler_for_over(int over_idx, bool high_intensity_previous_over, int previous_bowler) {
    if (high_intensity_previous_over) {
        int promoted = choose_death_specialist(previous_bowler);
        if (promoted != -1) return promoted;
    }
    return choose_round_robin_bowler(over_idx, previous_bowler);
}

void record_timeline_entry(const string& marker, const char* bowler_name, const char* striker_name,
                          const char* non_striker_name, const char* result) {
    TimelineEntry entry;
    entry.innings = ms.innings;
    entry.marker = marker;
    entry.bowler = bowler_name;
    entry.striker = striker_name;
    entry.non_striker = non_striker_name;
    entry.result = result;
    gantt_entries.push_back(entry);
}

void reset_ball_state_locked() {
    ms.bowler_turn = false;
    ms.striker_turn = false;
    ms.ball_resolved = false;
    ms.catch_window_open = false;
    ms.delivery_type = DELIVERY_FAIR;
    ms.dismissal_type = DISMISS_NONE;
    ms.runs_this_ball = 0;
    ms.batsman_runs_this_ball = 0;
    ms.runs_attempted_this_ball = 0;
    ms.dismissed_batsman_id = -1;
    ms.runout_survivor_id = -1;
    ms.wicket_this_ball = false;
    ms.extra_this_ball = false;
    ms.legal_ball_completed = false;
    ms.catch_taken = false;
    ms.catcher_id = -1;
    ms.play_result_log[0] = '\0';
    clear_run_graph();
}

void reset_players_for_match() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        teamA[i].state = WAITING;
        teamA[i].pending_activation = false;
        teamA[i].has_active_slot = false;
        teamA[i].held_end = -1;
        teamA[i].bat.runs_scored = 0;
        teamA[i].bat.balls_faced = 0;
        teamA[i].bat.wait_ms = 0;
        teamA[i].bat.batting_position = 0;
        teamA[i].bowl.wickets = 0;
        teamA[i].bowl.runs_conceded = 0;
        teamA[i].bowl.legal_balls = 0;
        teamA[i].bowl.overs_bowled = 0;

        teamB[i].state = WAITING;
        teamB[i].pending_activation = false;
        teamB[i].has_active_slot = false;
        teamB[i].held_end = -1;
        teamB[i].bat.runs_scored = 0;
        teamB[i].bat.balls_faced = 0;
        teamB[i].bat.wait_ms = 0;
        teamB[i].bat.batting_position = 0;
        teamB[i].bowl.wickets = 0;
        teamB[i].bowl.runs_conceded = 0;
        teamB[i].bowl.legal_balls = 0;
        teamB[i].bowl.overs_bowled = 0;
    }
}

int parse_team_name(const char* team_name) {
    if (team_name == nullptr) return -1;
    string wanted = lowercase_copy(team_name);

    for (int i = 0; i < AVAILABLE_TEAMS; i++) {
        if (wanted == lowercase_copy(team_labels[i])) return i;
    }
    return -1;
}

void load_team_from_template(Player* roster, int side_index, int country_id) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const PlayerTemplate& src = team_templates[country_id][i];
        roster[i].id = i;
        roster[i].team = side_index;
        roster[i].state = WAITING;
        roster[i].pending_activation = false;
        roster[i].has_active_slot = false;
        roster[i].held_end = -1;
        strncpy(roster[i].name, src.name, sizeof(roster[i].name) - 1);
        roster[i].name[sizeof(roster[i].name) - 1] = '\0';
        roster[i].bat.avg = src.bat_avg;
        roster[i].bowl.eco = src.bowl_eco;
    }
}

void init_teams() {
    load_team_from_template(teamA, 0, selected_country_ids[0]);
    load_team_from_template(teamB, 1, selected_country_ids[1]);
}

void maybe_release_active_slot(Player* self) {
    if (self->has_active_slot) {
        self->has_active_slot = false;
        sem_post(&S.active_batsmen_slots);
    }
}

string batsman_gantt_label(const Player* self) {
    if (self == nullptr) return string();
    if (ms.striker_id == self->id) return string("Striker: ") + self->name;
    if (ms.non_striker_id == self->id) return string("NonStriker: ") + self->name;
    return string();
}

string bowler_gantt_label(const Player* self) {
    if (self == nullptr) return string();
    if (ms.active_bowler_id == self->id) return string("Bowler: ") + self->name;
    return string();
}

void clear_process_requests_locked(int process) {
    if (process < 0 || process > 1) return;
    run_graph.request_edge[process][0] = false;
    run_graph.request_edge[process][1] = false;
}

void release_player_end_locked(Player* self, int process) {
    if (self->held_end == -1) return;
    int held_end = self->held_end;
    if (process >= 0 && process < 2) run_graph.allocation_edge[held_end][process] = false;
    pthread_mutex_unlock(pitch_end_mutex(held_end));
    self->held_end = -1;
}

void* batsman_thread(void* arg) {
    Player* self = static_cast<Player*>(arg);
    int last_shot_serial = -1;
    int last_run_serial = -1;
    int last_run_leg = -1;
    bool is_awake = false;

    while (true) {
        pthread_mutex_lock(&S.mutex_umpire);

        int cleanup_process = run_graph_player_index(self->id);
        if (self->held_end != -1 && (!run_graph.active || cleanup_process == -1 || ms.ball_resolved || ms.innings_complete)) {
            clear_process_requests_locked(cleanup_process);
            release_player_end_locked(self, cleanup_process);
        }

        while (!ms.innings_complete &&
               !(self->pending_activation && !self->has_active_slot) &&
               !(self->has_active_slot && ms.striker_turn && ms.striker_id == self->id && last_shot_serial != ms.ball_serial) &&
               !(self->has_active_slot && run_graph.active &&
                 (run_graph.player_ids[0] == self->id || run_graph.player_ids[1] == self->id) &&
                 (last_run_serial != run_graph.serial || last_run_leg != run_graph.current_leg))) {
            int wait_cleanup_process = run_graph_player_index(self->id);
            if (self->held_end != -1 && (!run_graph.active || wait_cleanup_process == -1 || ms.ball_resolved)) {
                clear_process_requests_locked(wait_cleanup_process);
                release_player_end_locked(self, wait_cleanup_process);
                continue;
            }
            
            pthread_cond_wait(&S.cond_batsman_wake, &S.mutex_umpire);
            
        }

        if (ms.innings_complete) {
            int process = run_graph_player_index(self->id);
            clear_process_requests_locked(process);
            release_player_end_locked(self, process);
            maybe_release_active_slot(self);
            pthread_mutex_unlock(&S.mutex_umpire);
            
            return nullptr;
        }

        if (self->pending_activation && !self->has_active_slot) {
            pthread_mutex_unlock(&S.mutex_umpire);
            
            sem_wait(&S.active_batsmen_slots);
            pthread_mutex_lock(&S.mutex_umpire);
            if (ms.innings_complete) {
                sem_post(&S.active_batsmen_slots);
                pthread_mutex_unlock(&S.mutex_umpire);
                
                return nullptr;
            }
            self->pending_activation = false;
            self->has_active_slot = true;
            self->state = BATTING;
            self->bat.wait_ms = elapsed_ms_since(innings_start_clock);
            pthread_cond_broadcast(&S.cond_batsman_wake);
            
            pthread_mutex_unlock(&S.mutex_umpire);
            continue;
        }

        if (self->has_active_slot && ms.striker_turn && ms.striker_id == self->id && last_shot_serial != ms.ball_serial) {
            record_gantt_event(string("Striker: ") + self->name);
            int shot_serial = ms.ball_serial;
            bool free_hit = ms.free_hit_active;
            DeliveryType delivery = ms.delivery_type;
            double batting_avg = self->bat.avg;
            double bowling_eco = bowling_team[ms.active_bowler_id].bowl.eco;
            pthread_mutex_unlock(&S.mutex_umpire);

            
            usleep(random_delay_us(5000, 11000));
            pthread_mutex_lock(&S.mutex_umpire);
            
            pthread_mutex_unlock(&S.mutex_umpire);
            int runs_attempted = choose_runs_attempted(batting_avg, bowling_eco);
            bool lofted = choose_lofted_boundary(runs_attempted, batting_avg, bowling_eco);

            pthread_mutex_lock(&S.mutex_umpire);
            if (ms.innings_complete || ms.ball_serial != shot_serial || !ms.striker_turn || ms.striker_id != self->id) {
                pthread_mutex_unlock(&S.mutex_umpire);
                continue;
            }

            last_shot_serial = shot_serial;
            ms.striker_turn = false;
            ms.runs_attempted_this_ball = runs_attempted;

            if (runs_attempted == 0) {
                ms.runs_this_ball = (delivery == DELIVERY_NO_BALL) ? 1 : 0;
                ms.batsman_runs_this_ball = 0;
                ms.ball_resolved = true;
                snprintf(ms.play_result_log, sizeof(ms.play_result_log), delivery == DELIVERY_NO_BALL ? "No ball + dot" : "Dot ball");
                pthread_cond_signal(&S.cond_umpire_wake);
            } else if (runs_attempted == 4 || runs_attempted == 6) {
                if (lofted && !free_hit) {
                    ms.catch_window_open = true;
                    pthread_cond_broadcast(&S.cond_fielders_wake);
                    pthread_cond_broadcast(&S.cond_bowler_wake);
                    pthread_mutex_unlock(&S.mutex_umpire);
                    
                    usleep(random_delay_us(7000, 14000));
                    pthread_mutex_lock(&S.mutex_umpire);
                    
                    ms.catch_window_open = false;
                }

                if (ms.catch_taken && !free_hit) {
                    ms.wicket_this_ball = true;
                    ms.dismissal_type = DISMISS_CAUGHT;
                    ms.dismissed_batsman_id = self->id;
                    ms.runs_this_ball = 0;
                    ms.batsman_runs_this_ball = 0;
                    ms.ball_resolved = true;
                    snprintf(ms.play_result_log, sizeof(ms.play_result_log), "OUT! Caught by %s", bowling_team[ms.catcher_id].name);
                    pthread_cond_signal(&S.cond_umpire_wake);
                } else {
                    ms.runs_this_ball = runs_attempted + ((delivery == DELIVERY_NO_BALL) ? 1 : 0);
                    ms.batsman_runs_this_ball = runs_attempted;
                    ms.ball_resolved = true;
                    if (delivery == DELIVERY_NO_BALL) snprintf(ms.play_result_log, sizeof(ms.play_result_log), "No ball + %d runs", runs_attempted);
                    else snprintf(ms.play_result_log, sizeof(ms.play_result_log), "%d Runs! Boundary", runs_attempted);
                    pthread_cond_signal(&S.cond_umpire_wake);
                }
            } else {
                start_run_graph(runs_attempted);
                pthread_cond_broadcast(&S.cond_batsman_wake);
            }

            pthread_mutex_unlock(&S.mutex_umpire);
            continue;
        }

        if (self->has_active_slot && run_graph.active &&
            (run_graph.player_ids[0] == self->id || run_graph.player_ids[1] == self->id) &&
            (last_run_serial != run_graph.serial || last_run_leg != run_graph.current_leg)) {
            string s1 = batting_team[run_graph.player_ids[0]].name;
            string s2 = batting_team[run_graph.player_ids[1]].name;
            record_gantt_event(
                string("Run: ") + s1 + " <-> " + s2
            );

            int run_serial = run_graph.serial;
            int leg = run_graph.current_leg;
            pthread_mutex_unlock(&S.mutex_umpire);

            
            usleep(random_delay_us(4000, 10000));
            pthread_mutex_lock(&S.mutex_umpire);
            
            pthread_mutex_unlock(&S.mutex_umpire);

            pthread_mutex_lock(&S.mutex_umpire);
            if (!run_graph.active || run_graph.serial != run_serial || run_graph.current_leg != leg || ms.innings_complete) {
                pthread_mutex_unlock(&S.mutex_umpire);
                continue;
            }

            int process = run_graph_player_index(self->id);
            if (process == -1) {
                pthread_mutex_unlock(&S.mutex_umpire);
                continue;
            }

            last_run_serial = run_serial;
            last_run_leg = leg;

            int current_end = run_graph.player_end[process];
            int target_end = 1 - current_end;
            if (self->held_end != current_end) {
                pthread_mutex_unlock(&S.mutex_umpire);
                pthread_mutex_lock(pitch_end_mutex(current_end));
                pthread_mutex_lock(&S.mutex_umpire);

                if (!run_graph.active || run_graph.serial != run_serial || run_graph.current_leg != leg || ms.innings_complete) {
                    pthread_mutex_unlock(pitch_end_mutex(current_end));
                    pthread_mutex_unlock(&S.mutex_umpire);
                    continue;
                }

                self->held_end = current_end;
                run_graph.allocation_edge[current_end][process] = true;
                run_graph.leg_ready[process] = true;
                pthread_cond_broadcast(&S.cond_batsman_wake);
                pthread_cond_signal(&S.cond_umpire_wake);
            }
            if (self->held_end == current_end && !run_graph.leg_ready[process]) {
                run_graph.leg_ready[process] = true;
                pthread_cond_broadcast(&S.cond_batsman_wake);
            }

            int other_process = 1 - process;
            while (run_graph.active && run_graph.serial == run_serial && run_graph.current_leg == leg &&
                   !run_graph.leg_ready[other_process] && !run_graph.deadlock_detected && !ms.innings_complete) {
                
                    pthread_cond_wait(&S.cond_batsman_wake, &S.mutex_umpire);
                
                }

            if (!run_graph.active || run_graph.serial != run_serial || run_graph.current_leg != leg || ms.innings_complete) {
                release_player_end_locked(self, process);
                pthread_mutex_unlock(&S.mutex_umpire);
                continue;
            }

            bool hold_and_request = (randf() < 0.11);
            if (!hold_and_request) release_player_end_locked(self, process);

            clear_process_requests_locked(process);
            run_graph.request_edge[process][target_end] = true;
            pthread_cond_signal(&S.cond_umpire_wake);
            pthread_mutex_unlock(&S.mutex_umpire);

            while (true) {
                if (pthread_mutex_trylock(pitch_end_mutex(target_end)) == 0) {
                    pthread_mutex_lock(&S.mutex_umpire);

                    if (!run_graph.active || run_graph.serial != run_serial || ms.innings_complete) {
                        pthread_mutex_unlock(pitch_end_mutex(target_end));
                        pthread_mutex_unlock(&S.mutex_umpire);
                        break;
                    }

                    clear_process_requests_locked(process);
                    if (hold_and_request && self->held_end == current_end) release_player_end_locked(self, process);

                    self->held_end = target_end;
                    run_graph.allocation_edge[target_end][process] = true;
                    run_graph.player_end[process] = target_end;
                    run_graph.leg_complete[process] = true;

                    if (run_graph.deadlock_detected && self->id == run_graph.deadlock_survivor_id && run_graph.victim_released) {
                        ms.runout_survivor_id = self->id;
                        ms.ball_resolved = true;
                        run_graph.active = false;
                        pthread_cond_broadcast(&S.cond_batsman_wake);
                        pthread_cond_signal(&S.cond_umpire_wake);
                    } else {
                        maybe_finish_running_ball_locked();
                        pthread_cond_signal(&S.cond_umpire_wake);
                    }

                    pthread_mutex_unlock(&S.mutex_umpire);
                    break;
                }

                
                usleep(500);
                pthread_mutex_lock(&S.mutex_umpire);
                
                pthread_mutex_unlock(&S.mutex_umpire);
                pthread_mutex_lock(&S.mutex_umpire);

                if (!run_graph.active || run_graph.serial != run_serial || ms.innings_complete) {
                    clear_process_requests_locked(process);
                    release_player_end_locked(self, process);
                    pthread_mutex_unlock(&S.mutex_umpire);
                    break;
                }

                if (run_graph.deadlock_detected && run_graph.deadlock_victim_id == self->id) {
                    clear_process_requests_locked(process);
                    release_player_end_locked(self, process);
                    run_graph.victim_released = true;
                    self->state = OUT_STATE;
                    pthread_cond_broadcast(&S.cond_batsman_wake);
                    pthread_cond_signal(&S.cond_umpire_wake);
                    pthread_mutex_unlock(&S.mutex_umpire);
                    break;
                }

                pthread_mutex_unlock(&S.mutex_umpire);
            }

            continue;
        }

        pthread_mutex_unlock(&S.mutex_umpire);
    }
}

void* fielding_player_thread(void* arg) {
    Player* self = static_cast<Player*>(arg);
    int last_delivery_serial = -1;
    int last_catch_serial = -1;
    bool is_awake = false;

    while (true) {
        pthread_mutex_lock(&S.mutex_umpire);
        while (!ms.innings_complete &&
               !((ms.active_bowler_id == self->id) && ms.bowler_turn && last_delivery_serial != ms.ball_serial) &&
               !((ms.active_bowler_id != self->id) && ms.catch_window_open && last_catch_serial != ms.ball_serial)) {
            
            pthread_cond_wait(&S.cond_bowler_wake, &S.mutex_umpire);
            
        }

        if (ms.innings_complete) {
            pthread_mutex_unlock(&S.mutex_umpire);
            
            return nullptr;
        }

        if (ms.active_bowler_id == self->id && ms.bowler_turn && last_delivery_serial != ms.ball_serial) {
            record_gantt_event(string("Bowler: ") + self->name);
            int delivery_serial = ms.ball_serial;
            double bowling_eco = self->bowl.eco;
            double striker_batting_avg = batting_team[ms.striker_id].bat.avg;
            pthread_mutex_unlock(&S.mutex_umpire);

            
            usleep(random_delay_us(5000, 12000));
            pthread_mutex_lock(&S.mutex_umpire);
            
            pthread_mutex_unlock(&S.mutex_umpire);
            double wide_probability = 0.0;
            double no_ball_probability = 0.0;
            double wicket_probability = 0.0;
            compute_delivery_probabilities(striker_batting_avg, bowling_eco,
                                           wide_probability, no_ball_probability, wicket_probability);
            double delivery_roll = randf();

            pthread_mutex_lock(&S.mutex_umpire);
            if (ms.innings_complete || ms.ball_serial != delivery_serial || !ms.bowler_turn || ms.active_bowler_id != self->id) {
                pthread_mutex_unlock(&S.mutex_umpire);
                continue;
            }

            last_delivery_serial = delivery_serial;
            ms.bowler_turn = false;

            if (delivery_roll < wide_probability) {
                ms.delivery_type = DELIVERY_WIDE;
                ms.extra_this_ball = true;
                ms.runs_this_ball = 1;
                ms.batsman_runs_this_ball = 0;
                ms.ball_resolved = true;
                snprintf(ms.play_result_log, sizeof(ms.play_result_log), "Wide ball");
                pthread_cond_signal(&S.cond_umpire_wake);
            } else if (delivery_roll < wide_probability + no_ball_probability) {
                ms.delivery_type = DELIVERY_NO_BALL;
                ms.extra_this_ball = true;
                ms.free_hit_next_ball = true;
                ms.striker_turn = true;
                pthread_cond_broadcast(&S.cond_batsman_wake);
            } else if (!ms.free_hit_active && delivery_roll < wide_probability + no_ball_probability + wicket_probability) {
                ms.delivery_type = DELIVERY_BOWLED;
                ms.wicket_this_ball = true;
                ms.dismissal_type = DISMISS_BOWLED;
                ms.dismissed_batsman_id = ms.striker_id;
                ms.runs_this_ball = 0;
                ms.batsman_runs_this_ball = 0;
                ms.ball_resolved = true;
                snprintf(ms.play_result_log, sizeof(ms.play_result_log), "OUT! Clean bowled/LBW");
                pthread_cond_signal(&S.cond_umpire_wake);
            } else {
                ms.delivery_type = DELIVERY_FAIR;
                ms.striker_turn = true;
                pthread_cond_broadcast(&S.cond_batsman_wake);
            }

            pthread_mutex_unlock(&S.mutex_umpire);
            continue;
        }

        if (ms.active_bowler_id != self->id && ms.catch_window_open && last_catch_serial != ms.ball_serial) {
            int catch_serial = ms.ball_serial;
            double striker_batting_avg = batting_team[ms.striker_id].bat.avg;
            double bowling_eco = bowling_team[ms.active_bowler_id].bowl.eco;
            pthread_mutex_unlock(&S.mutex_umpire);

            
            usleep(random_delay_us(3000, 9000));
            pthread_mutex_lock(&S.mutex_umpire);
            
            pthread_mutex_unlock(&S.mutex_umpire);
            bool attempt_success = choose_catch_success(striker_batting_avg, bowling_eco);

            pthread_mutex_lock(&S.mutex_umpire);
            if (!ms.catch_window_open || ms.ball_serial != catch_serial || ms.catch_taken || ms.innings_complete) {
                pthread_mutex_unlock(&S.mutex_umpire);
                continue;
            }

            last_catch_serial = catch_serial;
            if (attempt_success) {
                pthread_mutex_lock(&S.mutex_catch);
                if (!ms.catch_taken) {
                    ms.catch_taken = true;
                    ms.catcher_id = self->id;
                }
                pthread_mutex_unlock(&S.mutex_catch);
            }
            pthread_mutex_unlock(&S.mutex_umpire);
        }
    }
}

void* umpire_thread(void* arg) {
    bool use_sjf = *static_cast<bool*>(arg);
    build_batting_schedule(use_sjf);
    for (int i = 0; i < MAX_PLAYERS; i++) innings_batting_order[ms.innings][i] = batting_order[i];
    innings_use_sjf[ms.innings] = use_sjf;
    build_bowler_schedule();
    bool is_awake = false;
    

    gantt_start_clock = chrono::steady_clock::now();
    current_gantt_innings = ms.innings;

    pthread_mutex_lock(&S.mutex_umpire);
    ms.next_bat_idx = 0;
    activate_batsman_locked(ms.next_bat_idx++);
    activate_batsman_locked(ms.next_bat_idx++);
    ms.striker_id = batting_order[0];
    ms.non_striker_id = batting_order[1];
    pthread_mutex_unlock(&S.mutex_umpire);
    bool high_intensity_previous_over = false;
    int previous_bowler = -1;

    for (int over = 0; over < TOTAL_OVERS; over++) {
        if (ms.wickets[ms.innings] >= MAX_PLAYERS - 1) break;
        if (ms.innings == 1 && ms.scores[1] > ms.target) break;

        int bowler_id = select_bowler_for_over(over, high_intensity_previous_over, previous_bowler);
        bowler_rotation[over] = bowler_id;
        ms.active_bowler_id = bowler_id;
        ms.current_over = over;
        ms.legal_balls_in_over = 0;

        print_over_header(over + 1, bowling_team[bowler_id].name, ms.scores[ms.innings], ms.wickets[ms.innings], ms.free_hit_next_ball);

        int over_runs = 0;
        int over_wickets = 0;

        while (ms.legal_balls_in_over < BALLS_PER_OVER) {
            if (ms.wickets[ms.innings] >= MAX_PLAYERS - 1) break;
            if (ms.innings == 1 && ms.scores[1] > ms.target) break;

            pthread_mutex_lock(&S.mutex_umpire);
            reset_ball_state_locked();
            ms.ball_serial++;
            ms.free_hit_active = ms.free_hit_next_ball;
            ms.free_hit_next_ball = false;
            record_gantt_event("Umpire");
            ms.bowler_turn = true;
            char striker_name[32];
            char non_striker_name[32];
            strncpy(striker_name, batting_team[ms.striker_id].name, sizeof(striker_name) - 1);
            striker_name[sizeof(striker_name) - 1] = '\0';
            strncpy(non_striker_name, batting_team[ms.non_striker_id].name, sizeof(non_striker_name) - 1);
            non_striker_name[sizeof(non_striker_name) - 1] = '\0';

            pthread_cond_broadcast(&S.cond_bowler_wake);
            while (!ms.ball_resolved) {
                if (run_graph.active && !run_graph.deadlock_detected && run_graph_has_cycle()) {
                    declare_run_out_deadlock_locked();
                    continue;
                }
                
                pthread_cond_wait(&S.cond_umpire_wake, &S.mutex_umpire);
                
            }

            int legal_balls_before = ms.legal_balls_in_over;
            if (!ms.extra_this_ball) ms.legal_balls_in_over++;

            int score_striker_id = ms.striker_id;
            if (ms.wicket_this_ball && ms.dismissed_batsman_id >= 0) score_striker_id = ms.dismissed_batsman_id;
            Player* score_target = &batting_team[score_striker_id];

            pthread_mutex_lock(&S.mutex_score);
            ms.scores[ms.innings] += ms.runs_this_ball;
            if (ms.wicket_this_ball) ms.wickets[ms.innings]++;

            Player* bowler = &bowling_team[ms.active_bowler_id];
            bowler->bowl.runs_conceded += ms.runs_this_ball;
            if (!ms.extra_this_ball) {
                bowler->bowl.legal_balls++;
                bowler->bowl.overs_bowled = bowler->bowl.legal_balls / 6;
            }
            if (ms.wicket_this_ball && ms.dismissal_type != DISMISS_RUN_OUT) bowler->bowl.wickets++;

            if (ms.delivery_type != DELIVERY_WIDE) score_target->bat.balls_faced++;
            score_target->bat.runs_scored += ms.batsman_runs_this_ball;
            pthread_mutex_unlock(&S.mutex_score);

            over_runs += ms.runs_this_ball;
            if (ms.wicket_this_ball) over_wickets++;

            string marker = format_ball_marker(over + 1, ms.extra_this_ball ? legal_balls_before : ms.legal_balls_in_over, ms.extra_this_ball);
            BallRenderInfo render_info = describe_ball_for_display(ms.free_hit_active);
            print_ball_event(marker, striker_name, non_striker_name, ms.scores[ms.innings], ms.wickets[ms.innings], render_info);

            record_gantt_event("Umpire");

            record_timeline_entry(marker, bowling_team[bowler_id].name, striker_name, non_striker_name, ms.play_result_log);

            if (ms.wicket_this_ball && ms.dismissed_batsman_id >= 0) {
                batting_team[ms.dismissed_batsman_id].state = OUT_STATE;
                maybe_release_active_slot(&batting_team[ms.dismissed_batsman_id]);
                if (ms.next_bat_idx < MAX_PLAYERS && ms.wickets[ms.innings] < MAX_PLAYERS - 1) {
                    if (ms.dismissal_type == DISMISS_RUN_OUT && ms.runout_survivor_id >= 0) {
                        ms.non_striker_id = ms.runout_survivor_id;
                        ms.striker_id = batting_order[ms.next_bat_idx];
                        record_gantt_event(
                            string("Striker: ") + batting_team[ms.striker_id].name +
                            ", NonStriker: " + batting_team[ms.non_striker_id].name
                        );
                        activate_batsman_locked(ms.next_bat_idx++);
                    } else {
                        ms.striker_id = batting_order[ms.next_bat_idx];
                        record_gantt_event(
                            string("Striker: ") + batting_team[ms.striker_id].name +
                            ", NonStriker: " + batting_team[ms.non_striker_id].name
                        );
                        activate_batsman_locked(ms.next_bat_idx++);
                    }
                }
            }
            pthread_mutex_unlock(&S.mutex_umpire);
            
            
            usleep(8000);
            
        }

        innings_reports[ms.innings].push_back({ms.innings, over + 1, ms.bowling_team_id, bowler_id, over_runs, over_wickets, over_runs >= 15});
        print_over_summary(over + 1, over_runs, over_wickets, ms.scores[ms.innings], ms.wickets[ms.innings]);

        high_intensity_previous_over = (over_runs >= 15);
        previous_bowler = bowler_id;

        if (ms.wickets[ms.innings] >= MAX_PLAYERS - 1) break;
        if (ms.innings == 1 && ms.scores[1] > ms.target) break;

        if (ms.legal_balls_in_over == BALLS_PER_OVER) {
            pthread_mutex_lock(&S.mutex_umpire);
            int temp = ms.striker_id;
            ms.striker_id = ms.non_striker_id;
            ms.non_striker_id = temp;
            pthread_mutex_unlock(&S.mutex_umpire);
        }
    }

    pthread_mutex_lock(&S.mutex_umpire);
    ms.innings_complete = true;
    pthread_cond_broadcast(&S.cond_batsman_wake);
    pthread_cond_broadcast(&S.cond_bowler_wake);
    pthread_cond_broadcast(&S.cond_fielders_wake);
    pthread_mutex_unlock(&S.mutex_umpire);
    
    return nullptr;
}

void print_gantt_report() {
    printf("\n");
    print_divider();
    printf(" GANTT CHART\n");
    print_divider();

    for (int innings = 0; innings < 2; innings++) {
        printf("\n Innings %d\n", innings + 1);
        printf(" %-10s | %-40s\n", "Time(us)", "Active Threads");
        printf("------------+----------------------------------------------\n");

        vector<GanttEvent> events;

        for (auto &e : gantt_events) {
            if (e.innings == innings) {
                events.push_back(e);
            }
        }

        if (events.empty()) {
            printf(" No events recorded.\n");
            continue;
        }

        sort(events.begin(), events.end(), [](const GanttEvent& a, const GanttEvent& b) {
            return a.time_us < b.time_us;
        });

        long long base_time = events[0].time_us;

        map<long long, set<string>> timeline;

        for (auto &e : events) {
            long long t = e.time_us - base_time;
            timeline[t].clear();
            timeline[t].insert(e.actor);
        }

        set<string> current_threads;
        long long last_time = 0;

        for (auto &entry : timeline) {
            long long t = entry.first;
            set<string> threads = entry.second;

            if (threads != current_threads) {
                printf(" %10lld | ", t);

                if (threads.empty()) {
                    printf("Idle");
                } else {
                    bool first = true;
                    for (auto &th : threads) {
                        if (!first) printf(", ");
                        printf("%s", th.c_str());
                        first = false;
                    }
                }
                printf("\n");

                current_threads = threads;
                last_time = t;
            }
        }

        printf(" %10lld | Idle\n", last_time + 1);
    }
}

void print_strategy_analysis(const char* strategy_name) {
    printf("\n");
    print_ui_rule('-');
    printf(" %s (%s)\n", styled("WAIT TIME ANALYSIS", UI_BOLD_CYAN).c_str(), strategy_name);
    print_ui_rule('-');

    for (int innings = 0; innings < 2; innings++) {
        int batting_side = innings_batting_team_id[innings];
        Player* roster = (batting_side == 0) ? teamA : teamB;
        const char* team_name = team_labels[selected_country_ids[batting_side]];
        

        printf(" Innings %d  |  %s  |  %s\n",
               innings + 1, styled(team_name, UI_BOLD_WHITE).c_str(),
               innings_use_sjf[innings] ? "SJF" : "FCFS");
        printf(" %-4s | %-17s | %-9s | %-8s | %s\n", "Pos", "Player", "Status", "Wait", "Runs(Balls)");
        printf("------+-------------------+-----------+----------+------------\n");

        for (int pos = 0; pos < MAX_PLAYERS; pos++) {
            int player_id = innings_batting_order[innings][pos];
            Player& player = roster[player_id];
            bool batted = (player.bat.batting_position > 0);

            

            if (batted) {
                printf(" %4d | %-17.17s | %-9s | %6lldms | %2d(%d)\n",
                       pos + 1, player.name, "Batted", player.bat.wait_ms,
                       player.bat.runs_scored, player.bat.balls_faced);
            } else {
                printf(" %4d | %-17.17s | %-9s | %-8s | %s\n",
                       pos + 1, player.name, "DNB", "-", "-");
            }
        }


        printf("\n");
    }
}

const char* team_name_from_side(int team_id) {
    return team_labels[selected_country_ids[team_id]];
}

double batsman_strike_rate(const Player& player) {
    if (player.bat.balls_faced == 0) return 0.0;
    return 100.0 * static_cast<double>(player.bat.runs_scored) / static_cast<double>(player.bat.balls_faced);
}

double bowler_economy_rate(const Player& player) {
    if (player.bowl.legal_balls == 0) return 0.0;
    return 6.0 * static_cast<double>(player.bowl.runs_conceded) / static_cast<double>(player.bowl.legal_balls);
}

string overs_text_from_balls(int legal_balls) {
    char out[16];
    snprintf(out, sizeof(out), "%d.%d", legal_balls / 6, legal_balls % 6);
    return string(out);
}

vector<SummaryPlayerRef> collect_batting_performers() {
    vector<SummaryPlayerRef> refs;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (teamA[i].bat.batting_position > 0 || teamA[i].bat.balls_faced > 0 || teamA[i].bat.runs_scored > 0) {
            refs.push_back({&teamA[i], 0});
        }
        if (teamB[i].bat.batting_position > 0 || teamB[i].bat.balls_faced > 0 || teamB[i].bat.runs_scored > 0) {
            refs.push_back({&teamB[i], 1});
        }
    }

    sort(refs.begin(), refs.end(), [](const SummaryPlayerRef& lhs, const SummaryPlayerRef& rhs) {
        if (lhs.player->bat.runs_scored != rhs.player->bat.runs_scored) {
            return lhs.player->bat.runs_scored > rhs.player->bat.runs_scored;
        }
        if (lhs.player->bat.balls_faced != rhs.player->bat.balls_faced) {
            return lhs.player->bat.balls_faced < rhs.player->bat.balls_faced;
        }
        return strcmp(lhs.player->name, rhs.player->name) < 0;
    });
    return refs;
}

vector<SummaryPlayerRef> collect_bowling_performers() {
    vector<SummaryPlayerRef> refs;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (teamA[i].bowl.legal_balls > 0) refs.push_back({&teamA[i], 0});
        if (teamB[i].bowl.legal_balls > 0) refs.push_back({&teamB[i], 1});
    }

    sort(refs.begin(), refs.end(), [](const SummaryPlayerRef& lhs, const SummaryPlayerRef& rhs) {
        if (lhs.player->bowl.wickets != rhs.player->bowl.wickets) {
            return lhs.player->bowl.wickets > rhs.player->bowl.wickets;
        }
        if (lhs.player->bowl.runs_conceded != rhs.player->bowl.runs_conceded) {
            return lhs.player->bowl.runs_conceded < rhs.player->bowl.runs_conceded;
        }
        if (lhs.player->bowl.legal_balls != rhs.player->bowl.legal_balls) {
            return lhs.player->bowl.legal_balls > rhs.player->bowl.legal_balls;
        }
        return strcmp(lhs.player->name, rhs.player->name) < 0;
    });
    return refs;
}

double player_match_impact(const Player& player) {
    double impact = static_cast<double>(player.bat.runs_scored);
    impact += static_cast<double>(player.bowl.wickets) * 24.0;
    impact += batsman_strike_rate(player) / 12.0;
    if (player.bowl.legal_balls > 0) {
        impact += max(0.0, 10.5 - bowler_economy_rate(player)) * 2.0;
    }
    return impact;
}

SummaryPlayerRef choose_man_of_the_match() {
    SummaryPlayerRef best = {&teamA[0], 0};
    bool found = false;

    for (int team_id = 0; team_id < TEAM_COUNT; team_id++) {
        Player* roster = (team_id == 0) ? teamA : teamB;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            const Player& player = roster[i];
            if (player.bat.batting_position == 0 && player.bat.runs_scored == 0 &&
                player.bat.balls_faced == 0 && player.bowl.legal_balls == 0 && player.bowl.wickets == 0) {
                continue;
            }
            if (!found) {
                best = {&player, team_id};
                found = true;
                continue;
            }

            double lhs = player_match_impact(player);
            double rhs = player_match_impact(*best.player);
            if (lhs > rhs + 1e-9) {
                best = {&player, team_id};
                continue;
            }
            if (abs(lhs - rhs) <= 1e-9) {
                if (player.bowl.wickets != best.player->bowl.wickets) {
                    if (player.bowl.wickets > best.player->bowl.wickets) best = {&player, team_id};
                } else if (player.bat.runs_scored != best.player->bat.runs_scored) {
                    if (player.bat.runs_scored > best.player->bat.runs_scored) best = {&player, team_id};
                }
            }
        }
    }

    return best;
}

void print_match_summary() {
    const char* innings0_team = team_labels[selected_country_ids[innings_batting_team_id[0]]];
    const char* innings1_team = team_labels[selected_country_ids[innings_batting_team_id[1]]];

    const char* winner = "Tie";
    char margin[80];
    if (ms.scores[1] > ms.scores[0]) {
        winner = innings1_team;
        snprintf(margin, sizeof(margin), "%s won by %d wickets", innings1_team, (MAX_PLAYERS - 1) - ms.wickets[1]);
    } else if (ms.scores[0] > ms.scores[1]) {
        winner = innings0_team;
        snprintf(margin, sizeof(margin), "%s won by %d runs", innings0_team, ms.scores[0] - ms.scores[1]);
    } else {
        snprintf(margin, sizeof(margin), "Match tied");
    }

    int best_runs = -1;
    OverReport best_report = {};
    for (int innings = 0; innings < 2; innings++) {
        for (const OverReport& report : innings_reports[innings]) {
            if (report.runs > best_runs) {
                best_runs = report.runs;
                best_report = report;
            }
        }
    }

    vector<SummaryPlayerRef> top_batters = collect_batting_performers();
    vector<SummaryPlayerRef> top_bowlers = collect_bowling_performers();
    SummaryPlayerRef man_of_the_match = choose_man_of_the_match();

    printf("\n");
    print_ui_rule('=');
    printf(" %s\n", styled("MATCH SUMMARY", UI_BOLD_CYAN).c_str());
    print_ui_rule('=');
    printf(" %-12s %3d/%d\n", innings0_team, ms.scores[0], ms.wickets[0]);
    printf(" %-12s %3d/%d\n", innings1_team, ms.scores[1], ms.wickets[1]);
    printf(" %s %s\n", styled("Winner  ", UI_BOLD_YELLOW).c_str(), styled(winner, UI_BOLD_GREEN).c_str());
    printf(" %s %s\n", styled("Result  ", UI_BOLD_WHITE).c_str(), margin);
    if (best_runs >= 0) {
        const char* bowling_name = team_labels[selected_country_ids[best_report.bowling_team_id]];
        const char* batting_name = team_labels[selected_country_ids[innings_batting_team_id[best_report.innings]]];
        Player* report_bowling_team = (best_report.bowling_team_id == 0) ? teamA : teamB;
        printf(" %s Innings %d, Over %02d  |  %s scored %d off %s (%s)\n",
               styled("Highest-Scoring Over", UI_BOLD_WHITE).c_str(),
               best_report.innings + 1, best_report.over_number,
               batting_name, best_report.runs,
               report_bowling_team[best_report.bowler_id].name, bowling_name);
    }

    if (!top_batters.empty()) {
        const SummaryPlayerRef& top = top_batters.front();
        printf(" %s %s (%s)  %d(%d)  SR %.1f\n",
               styled("Highest Score", UI_BOLD_WHITE).c_str(),
               top.player->name, team_name_from_side(top.team_id),
               top.player->bat.runs_scored, top.player->bat.balls_faced,
               batsman_strike_rate(*top.player));
    }

    if (!top_bowlers.empty()) {
        const SummaryPlayerRef& top = top_bowlers.front();
        printf(" %s %s (%s)  %d/%d in %s overs  Econ %.2f\n",
               styled("Best Bowler ", UI_BOLD_WHITE).c_str(),
               top.player->name, team_name_from_side(top.team_id),
               top.player->bowl.wickets, top.player->bowl.runs_conceded,
               overs_text_from_balls(top.player->bowl.legal_balls).c_str(),
               bowler_economy_rate(*top.player));
    }

    printf(" %s %s (%s)",
           styled("Player of the Match", UI_BOLD_YELLOW).c_str(),
           man_of_the_match.player->name, team_name_from_side(man_of_the_match.team_id));
    if (man_of_the_match.player->bat.balls_faced > 0) {
        printf("  %d(%d)", man_of_the_match.player->bat.runs_scored, man_of_the_match.player->bat.balls_faced);
    }
    if (man_of_the_match.player->bowl.legal_balls > 0) {
        printf("  |  %d/%d in %s overs",
               man_of_the_match.player->bowl.wickets,
               man_of_the_match.player->bowl.runs_conceded,
               overs_text_from_balls(man_of_the_match.player->bowl.legal_balls).c_str());
    }
    printf("\n");

    if (!top_batters.empty()) {
        printf("\n %s\n", styled("Top Batters", UI_BOLD_CYAN).c_str());
        printf(" %-2s | %-17s | %-10s | %-11s | %6s\n", "#", "Player", "Team", "Runs(Balls)", "SR");
        printf("----+-------------------+------------+-------------+--------\n");
        int limit = min(3, static_cast<int>(top_batters.size()));
        for (int i = 0; i < limit; i++) {
            const SummaryPlayerRef& ref = top_batters[i];
            printf(" %2d | %-17.17s | %-10.10s | %3d(%-3d)    | %6.1f\n",
                   i + 1, ref.player->name, team_name_from_side(ref.team_id),
                   ref.player->bat.runs_scored, ref.player->bat.balls_faced,
                   batsman_strike_rate(*ref.player));
        }
    }

    if (!top_bowlers.empty()) {
        printf("\n %s\n", styled("Top Bowlers", UI_BOLD_CYAN).c_str());
        printf(" %-2s | %-17s | %-10s | %-9s | %-5s | %6s\n", "#", "Player", "Team", "Figures", "Overs", "Econ");
        printf("----+-------------------+------------+-----------+-------+--------\n");
        int limit = min(3, static_cast<int>(top_bowlers.size()));
        for (int i = 0; i < limit; i++) {
            const SummaryPlayerRef& ref = top_bowlers[i];
            printf(" %2d | %-17.17s | %-10.10s | %2d/%-6d | %-5s | %6.2f\n",
                   i + 1, ref.player->name, team_name_from_side(ref.team_id),
                   ref.player->bowl.wickets, ref.player->bowl.runs_conceded,
                   overs_text_from_balls(ref.player->bowl.legal_balls).c_str(),
                   bowler_economy_rate(*ref.player));
        }
    }
}

int fallback_team_for_slot(int slot, int other_team) {
    int preferred = (slot == 0) ? COUNTRY_INDIA : COUNTRY_AUSTRALIA;
    if (preferred != other_team) return preferred;
    for (int i = 0; i < AVAILABLE_TEAMS; i++) {
        if (i != other_team) return i;
    }
    return preferred;
}

int prompt_menu_choice(const char* title, const char* prompt, const char* const* options, int count,
                       int default_choice, int disallowed_choice = -1) {
    char buffer[64];
    while (true) {
        printf("\n %s\n", styled(title, UI_BOLD_CYAN).c_str());
        for (int i = 0; i < count; i++) {
            bool disabled = (i == disallowed_choice);
            printf("  %d. %s%s\n", i + 1, options[i], disabled ? " (already selected)" : "");
        }
        printf(" %s [%d]: ", prompt, default_choice + 1);
        if (fgets(buffer, sizeof(buffer), stdin) == nullptr) return default_choice;
        if (buffer[0] == '\n' || buffer[0] == '\0') return default_choice;

        char* end = nullptr;
        long parsed = strtol(buffer, &end, 10);
        if (end == buffer || (*end != '\n' && *end != '\0') || parsed < 1 || parsed > count) {
            printf(" %s Enter a number between 1 and %d.\n", styled("Invalid.", UI_BOLD_RED).c_str(), count);
            continue;
        }

        int chosen = static_cast<int>(parsed - 1);
        if (chosen == disallowed_choice) {
            printf(" %s Pick a different team.\n", styled("Conflict.", UI_BOLD_RED).c_str());
            continue;
        }
        return chosen;
    }
}

bool prompt_strategy_selection() {
    static const char* strategy_options[2] = {"FCFS batting order", "SJF batting order"};
    int choice = prompt_menu_choice("Batting Strategy", "Choose mode", strategy_options, 2, 0);
    return choice == 1;
}

void finalize_cli_config(CliConfig& config) {
    if (config.team_explicit[0] && config.team_explicit[1] && selected_country_ids[0] == selected_country_ids[1]) {
        printf("Team A and Team B must be different.\n");
        exit(1);
    }

    bool needs_prompt = ui_stdin_is_tty &&
        (!config.team_explicit[0] || !config.team_explicit[1] || !config.strategy_explicit);

    if (needs_prompt) {
        print_ui_rule('=');
        printf(" %s\n", styled("MATCH SETUP", UI_BOLD_CYAN).c_str());
        print_ui_rule('=');
        printf(" %s Choose teams and batting order before the match starts.\n",
               styled("Guide ", UI_BOLD_WHITE).c_str());
    }

    if (ui_stdin_is_tty) {
        if (!config.team_explicit[0]) {
            int default_team = fallback_team_for_slot(0, config.team_explicit[1] ? selected_country_ids[1] : -1);
            selected_country_ids[0] = prompt_menu_choice("Team A", "Select team", team_labels,
                                                         AVAILABLE_TEAMS, default_team,
                                                         config.team_explicit[1] ? selected_country_ids[1] : -1);
        }
        if (!config.team_explicit[1]) {
            int default_team = fallback_team_for_slot(1, selected_country_ids[0]);
            selected_country_ids[1] = prompt_menu_choice("Team B", "Select team", team_labels,
                                                         AVAILABLE_TEAMS, default_team, selected_country_ids[0]);
        }
        if (!config.strategy_explicit) config.use_sjf = prompt_strategy_selection();
    } else {
        if (!config.team_explicit[0]) {
            selected_country_ids[0] = fallback_team_for_slot(0, config.team_explicit[1] ? selected_country_ids[1] : -1);
        }
        if (!config.team_explicit[1]) {
            selected_country_ids[1] = fallback_team_for_slot(1, selected_country_ids[0]);
        }
    }

    if (selected_country_ids[0] == selected_country_ids[1]) {
        printf("Team A and Team B must be different.\n");
        exit(1);
    }
}

void parse_cli(int argc, char** argv, CliConfig& config) {
    config.use_sjf = false;
    config.team_explicit[0] = false;
    config.team_explicit[1] = false;
    config.strategy_explicit = false;
    selected_country_ids[0] = COUNTRY_INDIA;
    selected_country_ids[1] = COUNTRY_AUSTRALIA;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sjf") == 0) {
            config.use_sjf = true;
            config.strategy_explicit = true;
        }
        else if (strcmp(argv[i], "--team-a") == 0 || strcmp(argv[i], "--team-b") == 0) {
            if (i + 1 >= argc) {
                printf("Missing value for %s\n", argv[i]);
                printf("Use --help for usage.\n");
                exit(1);
            }

            int parsed = parse_team_name(argv[i + 1]);
            if (parsed == -1) {
                printf("Unknown team: %s\n", argv[i + 1]);
                printf("Valid teams: india, pakistan, england, australia\n");
                exit(1);
            }

            int slot = (strcmp(argv[i], "--team-a") == 0) ? 0 : 1;
            selected_country_ids[slot] = parsed;
            config.team_explicit[slot] = true;
            i++;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--sjf] [--team-a TEAM] [--team-b TEAM]\n", argv[0]);
            printf("  default     : guided setup in a terminal, or FCFS India vs Australia when non-interactive\n");
            printf("  --sjf       : shortest-job-first batting order by batting average\n");
            printf("  --team-a X  : choose Team A from india, pakistan, england, australia\n");
            printf("  --team-b X  : choose Team B from india, pakistan, england, australia\n");
            exit(0);
        } else {
            printf("Unknown option: %s\n", argv[i]);
            printf("Use --help for usage.\n");
            exit(1);
        }
    }
}

void launch_innings_threads(pthread_t& umpire, bool use_sjf) {
    current_innings_use_sjf = use_sjf;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pthread_create(&batting_team[i].thread, nullptr, batsman_thread, &batting_team[i]);
        pthread_create(&bowling_team[i].thread, nullptr, fielding_player_thread, &bowling_team[i]);
    }
    pthread_create(&umpire, nullptr, umpire_thread, &current_innings_use_sjf);
}

void join_innings_threads(pthread_t& umpire) {
    pthread_join(umpire, nullptr);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pthread_join(batting_team[i].thread, nullptr);
        pthread_join(bowling_team[i].thread, nullptr);
    }
}

void reset_match_state_for_innings(int innings, int batting_team_id, int bowling_team_id) {
    pthread_mutex_lock(&S.mutex_umpire);
    ms.innings = innings;
    ms.current_over = 0;
    ms.legal_balls_in_over = 0;
    ms.active_bowler_id = -1;
    ms.striker_id = -1;
    ms.non_striker_id = -1;
    ms.next_bat_idx = 0;
    ms.batting_team_id = batting_team_id;
    ms.bowling_team_id = bowling_team_id;
    ms.ball_serial = 0;
    ms.innings_complete = false;
    ms.bowler_turn = false;
    ms.striker_turn = false;
    ms.ball_resolved = false;
    ms.free_hit_next_ball = false;
    ms.free_hit_active = false;
    ms.catch_window_open = false;
    reset_ball_state_locked();
    pthread_mutex_unlock(&S.mutex_umpire);
    clear_run_graph();
    current_gantt_innings = innings;
    gantt_start_clock = chrono::steady_clock::now();

    for (int i = 0; i < MAX_PLAYERS; i++) {
        batting_team[i].state = WAITING;
        batting_team[i].pending_activation = false;
        batting_team[i].has_active_slot = false;
        batting_team[i].held_end = -1;
        batting_team[i].bat.wait_ms = 0;
        batting_team[i].bat.balls_faced = 0;
        batting_team[i].bat.runs_scored = 0;
        batting_team[i].bat.batting_position = 0;

        bowling_team[i].state = FIELDING;
        bowling_team[i].pending_activation = false;
        bowling_team[i].has_active_slot = false;
        bowling_team[i].held_end = -1;
        bowling_team[i].bowl.wickets = 0;
        bowling_team[i].bowl.runs_conceded = 0;
        bowling_team[i].bowl.legal_balls = 0;
        bowling_team[i].bowl.overs_bowled = 0;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    ui_stdout_is_tty = (isatty(STDOUT_FILENO) == 1);
    ui_stdin_is_tty = (isatty(STDIN_FILENO) == 1);
    ui_color_enabled = ui_stdout_is_tty && (getenv("NO_COLOR") == nullptr);

    random_device rd;
    unsigned seed = rd() ^
        static_cast<unsigned>(chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        static_cast<unsigned>(getpid());
    match_seed = seed;
    srand(seed);
    
    CliConfig config = {};
    parse_cli(argc, argv, config);
    finalize_cli_config(config);

    init_teams();
    reset_players_for_match();

    pthread_mutex_init(&S.mutex_umpire, nullptr);
    pthread_mutex_init(&S.mutex_score, nullptr);
    pthread_mutex_init(&S.mutex_catch, nullptr);
    pthread_mutex_init(&S.mutex_gantt, nullptr);
    pthread_mutex_init(&S.mutex_end_A, nullptr);
    pthread_mutex_init(&S.mutex_end_B, nullptr);
    pthread_cond_init(&S.cond_umpire_wake, nullptr);
    pthread_cond_init(&S.cond_batsman_wake, nullptr);
    pthread_cond_init(&S.cond_bowler_wake, nullptr);
    pthread_cond_init(&S.cond_fielders_wake, nullptr);
    sem_init(&S.active_batsmen_slots, 0, 2);

    memset(&ms, 0, sizeof(ms));
    clear_run_graph();
    gantt_entries.clear();
    gantt_events.clear();
    innings_reports[0].clear();
    innings_reports[1].clear();
    memset(innings_batting_order, 0, sizeof(innings_batting_order));
    innings_use_sjf[0] = innings_use_sjf[1] = config.use_sjf;
    gantt_start_clock = chrono::steady_clock::now();

    print_match_banner(config.use_sjf);

    int toss_winner = rand() % 2;
    int toss_choice = rand() % 2;
    int first_batting_team = (toss_choice == 0) ? toss_winner : (1 - toss_winner);
    int first_bowling_team = 1 - first_batting_team;

    print_toss_banner(toss_winner, toss_choice);

    innings_batting_team_id[0] = first_batting_team;
    innings_bowling_team_id[0] = first_bowling_team;
    innings_batting_team_id[1] = first_bowling_team;
    innings_bowling_team_id[1] = first_batting_team;

    batting_team = (first_batting_team == 0) ? teamA : teamB;
    bowling_team = (first_bowling_team == 0) ? teamA : teamB;
    reset_match_state_for_innings(0, first_batting_team, first_bowling_team);
    innings_start_clock = chrono::steady_clock::now();

    char innings1_title[64];
    snprintf(innings1_title, sizeof(innings1_title), "INNINGS 1  |  %s Batting",
             team_labels[selected_country_ids[first_batting_team]]);
    print_innings_header(innings1_title);

    pthread_t umpire;
    launch_innings_threads(umpire, config.use_sjf);
    join_innings_threads(umpire);

    ms.target = ms.scores[0];

    batting_team = (innings_batting_team_id[1] == 0) ? teamA : teamB;
    bowling_team = (innings_bowling_team_id[1] == 0) ? teamA : teamB;
    reset_match_state_for_innings(1, innings_batting_team_id[1], innings_bowling_team_id[1]);
    innings_start_clock = chrono::steady_clock::now();

    char innings2_title[64];
    snprintf(innings2_title, sizeof(innings2_title), "INNINGS 2  |  %s Batting",
             team_labels[selected_country_ids[innings_batting_team_id[1]]]);
    print_innings_header(innings2_title, ms.target + 1);

    launch_innings_threads(umpire, config.use_sjf);
    join_innings_threads(umpire);

    print_match_summary();
    print_strategy_analysis(config.use_sjf ? "SJF" : "FCFS");
    print_gantt_report();

    sem_destroy(&S.active_batsmen_slots);
    pthread_cond_destroy(&S.cond_umpire_wake);
    pthread_cond_destroy(&S.cond_batsman_wake);
    pthread_cond_destroy(&S.cond_bowler_wake);
    pthread_cond_destroy(&S.cond_fielders_wake);
    pthread_mutex_destroy(&S.mutex_umpire);
    pthread_mutex_destroy(&S.mutex_score);
    pthread_mutex_destroy(&S.mutex_catch);
    pthread_mutex_destroy(&S.mutex_gantt);
    pthread_mutex_destroy(&S.mutex_end_A);
    pthread_mutex_destroy(&S.mutex_end_B);
    return 0;
}
