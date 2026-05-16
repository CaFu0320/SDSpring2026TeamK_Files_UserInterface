#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <float.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <curl/curl.h>

#include <cms_v2x/api.h>
#include <cms_v2x/fac_types.h>
#include <cms_v2x/fac_subscribe.h>

#include <asn1/v2x_eu_asn.h>
#include <asn1/v2x_us_asn.h>
#include <asn1defs.h>

/*
    ============================================================
    SETTINGS
    ============================================================
*/

#define OBU_IP "192.168.1.54"

#define LOGIN_FILE "obu_login.txt"
#define COOKIE_FILE "cookies.txt"

static const char *filename = "log.txt";

enum { ASN_NAME_LENGTH = 64U };

#define MAX_TEXT 4096
#define MAX_TOKEN 256
#define MAX_URL 1024

#define MAX_SIGNAL_GROUPS   64
#define MAX_LANES_PER_GROUP 32

#define MAX_DISPLAY_SIGNALS 64
#define MAX_DISPLAY_LANES   32

/*
    ============================================================
    GPS TYPES
    ============================================================
*/

typedef struct {
    char *data;
    size_t size;
} Memory;

typedef struct {
    char username[128];
    char password[128];
} LoginInfo;

typedef struct {
    int is_valid;
    char source[64];

    long long timestamp_ms;
    long long raw_latitude;
    long long raw_longitude;
    long long raw_altitude;
    long long raw_heading;
    long long raw_speed;
    long long satellites_in_use;

    double latitude_deg;
    double longitude_deg;
    double altitude_m;
} GpsData;

/*
    ============================================================
    SHARED DISPLAY DATA

    GPS thread writes GPS data here.
    MAP/SPaT callback writes traffic-light data here.
    Main display loop reads from here.
    ============================================================
*/

typedef struct {
    /* GPS */
    int gps_valid;
    char gps_source[64];
    double latitude_deg;
    double longitude_deg;
    double altitude_m;

    long long raw_heading;
    double heading_deg;
    char cardinal_direction[32];

    long long raw_speed;
    long long satellites_in_use;

    /* Current lane estimate */
    int current_lane_valid;
    int current_lane_id;
    char current_lane_note[128];

    /* MAP */
    int map_valid;
    int intersection_id;
    char intersection_name[128];

    /* SPaT */
    int spat_valid;
    int signal_group_count;
    int signal_groups[MAX_DISPLAY_SIGNALS];
    char signal_colors[MAX_DISPLAY_SIGNALS][32];
    double signal_countdowns[MAX_DISPLAY_SIGNALS];

    int lane_count[MAX_DISPLAY_SIGNALS];
    int lanes[MAX_DISPLAY_SIGNALS][MAX_DISPLAY_LANES];

    /* Debug */
    unsigned int callback_count;
    int last_msg_type;
    unsigned int last_msg_length;

} SharedDisplayData;

static SharedDisplayData g_display;
static pthread_mutex_t g_data_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_running = 1;

/*
    ============================================================
    ASN TYPES
    ============================================================
*/

typedef struct {
    cms_fac_msg_type_t cms_type;
    char name[ASN_NAME_LENGTH];
    const ASN1CType* asn_type;
} asn_type_t;

static const asn_type_t asn_types[] = {
    { .cms_type = CMS_FAC_MSG_EU_CAM,  .name = "EU_CAM",  .asn_type = asn1_type_EU_CAM },
    { .cms_type = CMS_FAC_MSG_EU_DENM, .name = "EU_DENM", .asn_type = asn1_type_EU_DENM },
    { .cms_type = CMS_FAC_MSG_EU_MAP,  .name = "EU_MAP",  .asn_type = asn1_type_EU_MAP },
    { .cms_type = CMS_FAC_MSG_EU_SPAT, .name = "EU_SPAT", .asn_type = asn1_type_EU_SPAT },
    { .cms_type = CMS_FAC_MSG_EU_IVI,  .name = "EU_IVI",  .asn_type = asn1_type_EU_IVI },
    { .cms_type = CMS_FAC_MSG_EU_RTCM, .name = "EU_RTCM", .asn_type = asn1_type_EU_RTCM },
    { .cms_type = CMS_FAC_MSG_EU_SRM,  .name = "EU_SRM",  .asn_type = asn1_type_EU_SRM },
    { .cms_type = CMS_FAC_MSG_EU_SSM,  .name = "EU_SSM",  .asn_type = asn1_type_EU_SSM },

    { .cms_type = CMS_FAC_MSG_US_BSM,  .name = "US_BSM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_TIM,  .name = "US_TIM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_RSA,  .name = "US_RSA",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_MAP,  .name = "US_MAP",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_SPAT, .name = "US_SPAT", .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_SRM,  .name = "US_SRM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_SSM,  .name = "US_SSM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_PSM,  .name = "US_PSM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_PVD,  .name = "US_PVD",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_PDM,  .name = "US_PDM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_RTCM, .name = "US_RTCM", .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_WSA,  .name = "US_WSA",  .asn_type = asn1_type_US_MessageFrame },

    { .name = "", .asn_type = NULL }
};

typedef struct notif_ctx {
    uint32_t param;
    uint32_t cnt;
} notif_ctx_t;

/*
    ============================================================
    MAP SIGNAL GROUP → LANE STORAGE
    ============================================================
*/

typedef struct {
    int lane_count;
    int lanes[MAX_LANES_PER_GROUP];
} signal_group_lanes_t;

static signal_group_lanes_t g_signal_group_map[MAX_SIGNAL_GROUPS + 1];
static int g_last_intersection_id = -1;

/*
    ============================================================
    FORWARD DECLARATIONS
    ============================================================
*/

static const char* get_verify_result_string(cms_sec_verify_result_t verify_result_code);
static const ASN1CType* get_type(cms_fac_msg_type_t in_type);
static bool process_uper(cms_fac_msg_type_t type, cms_buffer_view_t msg);
static bool l_use_c_stuct(cms_fac_msg_type_t type, void* c_struct);
static bool l_print_xml_format(const ASN1CType* asn_type, void* c_struct);

static int obu_login(char *stok_out, int stok_size);
static int get_obu_gps(const char *stok, GpsData *gps);

/*
    ============================================================
    GPS HTTP FUNCTIONS
    ============================================================
*/

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    Memory *mem = (Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + real_size + 1);

    if (ptr == NULL) {
        printf("Memory error.\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = '\0';

    return real_size;
}

void remove_newline(char *s)
{
    s[strcspn(s, "\r\n")] = '\0';
}

int read_login_file(LoginInfo *login)
{
    FILE *fp;
    char line[256];

    login->username[0] = '\0';
    login->password[0] = '\0';

    fp = fopen(LOGIN_FILE, "r");

    if (fp == NULL) {
        printf("Could not open %s\n", LOGIN_FILE);
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        remove_newline(line);

        if (strncmp(line, "username=", 9) == 0) {
            strncpy(login->username, line + 9, sizeof(login->username) - 1);
            login->username[sizeof(login->username) - 1] = '\0';
        }

        if (strncmp(line, "password=", 9) == 0) {
            strncpy(login->password, line + 9, sizeof(login->password) - 1);
            login->password[sizeof(login->password) - 1] = '\0';
        }
    }

    fclose(fp);

    if (strlen(login->username) == 0 || strlen(login->password) == 0) {
        printf("Missing username or password in %s\n", LOGIN_FILE);
        return 0;
    }

    return 1;
}

int http_post_login(const LoginInfo *login, char **response_out)
{
    CURL *curl;
    CURLcode result;
    Memory chunk;
    char login_url[MAX_URL];
    char post_fields[MAX_TEXT];

    chunk.data = malloc(1);
    chunk.size = 0;

    if (chunk.data == NULL) {
        return 0;
    }

    snprintf(login_url, sizeof(login_url),
             "https://%s/cgi-bin/luci/", OBU_IP);

    snprintf(post_fields, sizeof(post_fields),
             "luci_username=%s&luci_password=%s",
             login->username, login->password);

    curl = curl_easy_init();

    if (curl == NULL) {
        free(chunk.data);
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, login_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);

    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, COOKIE_FILE);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, COOKIE_FILE);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    /* Do not follow redirect. We need Location header for ;stok=... */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    /* Include headers so we can find Location */
    curl_easy_setopt(curl, CURLOPT_HEADER, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    result = curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        printf("Login curl error: %s\n", curl_easy_strerror(result));
        free(chunk.data);
        return 0;
    }

    *response_out = chunk.data;
    return 1;
}

int extract_stok(const char *response, char *stok_out, int stok_size)
{
    const char *p;
    const char *start;
    const char *end;
    int len;

    p = strstr(response, ";stok=");

    if (p == NULL) {
        return 0;
    }

    start = p + strlen(";stok=");

    end = strchr(start, '/');

    if (end == NULL) {
        end = strchr(start, '\r');
    }

    if (end == NULL) {
        end = strchr(start, '\n');
    }

    if (end == NULL) {
        return 0;
    }

    len = end - start;

    if (len <= 0 || len >= stok_size) {
        return 0;
    }

    strncpy(stok_out, start, len);
    stok_out[len] = '\0';

    return 1;
}

static int obu_login(char *stok_out, int stok_size)
{
    LoginInfo login;
    char *response = NULL;
    int ok;

    if (!read_login_file(&login)) {
        return 0;
    }

    if (!http_post_login(&login, &response)) {
        return 0;
    }

    ok = extract_stok(response, stok_out, stok_size);

    if (!ok) {
        printf("Login worked, but could not find stok token.\n");
        printf("First part of response:\n\n");
        printf("%.500s\n", response);
        free(response);
        return 0;
    }

    free(response);
    return 1;
}

int http_get(const char *url, char **response_out)
{
    CURL *curl;
    CURLcode result;
    Memory chunk;

    chunk.data = malloc(1);
    chunk.size = 0;

    if (chunk.data == NULL) {
        return 0;
    }

    curl = curl_easy_init();

    if (curl == NULL) {
        free(chunk.data);
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, COOKIE_FILE);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, COOKIE_FILE);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    result = curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        printf("GET curl error: %s\n", curl_easy_strerror(result));
        free(chunk.data);
        return 0;
    }

    *response_out = chunk.data;
    return 1;
}

int extract_int(const char *json, const char *key, long long *value)
{
    char pattern[128];
    char *pos;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    pos = strstr(json, pattern);

    if (pos == NULL) {
        return 0;
    }

    pos += strlen(pattern);

    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }

    if (sscanf(pos, "%lld", value) == 1) {
        return 1;
    }

    return 0;
}

int extract_string(const char *json, const char *key, char *output, int output_size)
{
    char pattern[128];
    char *pos;
    char *start;
    char *end;
    int len;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    pos = strstr(json, pattern);

    if (pos == NULL) {
        return 0;
    }

    pos += strlen(pattern);

    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }

    start = strchr(pos, '\"');

    if (start == NULL) {
        return 0;
    }

    start++;

    end = strchr(start, '\"');

    if (end == NULL) {
        return 0;
    }

    len = end - start;

    if (len >= output_size) {
        len = output_size - 1;
    }

    strncpy(output, start, len);
    output[len] = '\0';

    return 1;
}

int extract_bool(const char *json, const char *key, int *value)
{
    char pattern[128];
    char *pos;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    pos = strstr(json, pattern);

    if (pos == NULL) {
        return 0;
    }

    pos += strlen(pattern);

    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }

    if (strncmp(pos, "true", 4) == 0) {
        *value = 1;
        return 1;
    }

    if (strncmp(pos, "false", 5) == 0) {
        *value = 0;
        return 1;
    }

    return 0;
}

double convert_lat_lon(long long raw)
{
    return raw / 10000000.0;
}

double convert_altitude(long long raw)
{
    return raw / 100.0;
}

double convert_heading_raw_to_degrees(long long raw_heading)
{
    /*
        The OBU web status appears to report heading in thousandths
        of a degree.

        Example:
            270000 -> 270.000 degrees -> Westbound
             90000 ->  90.000 degrees -> Eastbound
            180000 -> 180.000 degrees -> Southbound
              0000 ->   0.000 degrees -> Northbound
    */

    double heading_deg;

    if(raw_heading < 0) {
        return -1.0;
    }

    heading_deg = raw_heading / 1000.0;

    while(heading_deg >= 360.0) {
        heading_deg -= 360.0;
    }

    while(heading_deg < 0.0) {
        heading_deg += 360.0;
    }

    return heading_deg;
}

const char* heading_degrees_to_cardinal(double heading_deg)
{
    if(heading_deg < 0.0) {
        return "UNKNOWN";
    }

    if(heading_deg >= 337.5 || heading_deg < 22.5) {
        return "Northbound";
    } else if(heading_deg >= 22.5 && heading_deg < 67.5) {
        return "Northeastbound";
    } else if(heading_deg >= 67.5 && heading_deg < 112.5) {
        return "Eastbound";
    } else if(heading_deg >= 112.5 && heading_deg < 157.5) {
        return "Southeastbound";
    } else if(heading_deg >= 157.5 && heading_deg < 202.5) {
        return "Southbound";
    } else if(heading_deg >= 202.5 && heading_deg < 247.5) {
        return "Southwestbound";
    } else if(heading_deg >= 247.5 && heading_deg < 292.5) {
        return "Westbound";
    } else if(heading_deg >= 292.5 && heading_deg < 337.5) {
        return "Northwestbound";
    }

    return "UNKNOWN";
}

int parse_gps_json(const char *json, GpsData *gps)
{
    memset(gps, 0, sizeof(GpsData));
    strcpy(gps->source, "N/A");

    extract_bool(json, "isValid", &gps->is_valid);
    extract_string(json, "source", gps->source, sizeof(gps->source));

    extract_int(json, "timestampMs", &gps->timestamp_ms);
    extract_int(json, "latitude", &gps->raw_latitude);
    extract_int(json, "longitude", &gps->raw_longitude);
    extract_int(json, "altitude", &gps->raw_altitude);
    extract_int(json, "heading", &gps->raw_heading);
    extract_int(json, "speed", &gps->raw_speed);
    extract_int(json, "satellitesInUse", &gps->satellites_in_use);

    gps->latitude_deg = convert_lat_lon(gps->raw_latitude);
    gps->longitude_deg = convert_lat_lon(gps->raw_longitude);
    gps->altitude_m = convert_altitude(gps->raw_altitude);

    if (gps->raw_latitude == 0 && gps->raw_longitude == 0) {
        return 0;
    }

    return 1;
}

static int get_obu_gps(const char *stok, GpsData *gps)
{
    char url[MAX_URL];
    char *json = NULL;
    int ok;

    snprintf(url, sizeof(url),
             "https://%s/cgi-bin/luci/;stok=%s/admin/v2x_status/status?status=1",
             OBU_IP, stok);

    if (!http_get(url, &json)) {
        return 0;
    }

    ok = parse_gps_json(json, gps);

    free(json);
    return ok;
}

/*
    ============================================================
    GPS THREAD

    This runs in the background.
    It logs in once, then keeps updating GPS data.
    ============================================================
*/

void* gps_thread_func(void* arg)
{
    char stok[MAX_TOKEN];
    GpsData gps;
    int fail_count = 0;

    (void)arg;

    printf("GPS thread: logging into OBU web status...\n");

    if (!obu_login(stok, sizeof(stok))) {
        printf("GPS thread: login failed.\n");
        return NULL;
    }

    printf("GPS thread: login successful. stok = %.12s...\n", stok);

    while (g_running) {
        if (get_obu_gps(stok, &gps)) {
            fail_count = 0;

            pthread_mutex_lock(&g_data_mutex);

            g_display.gps_valid = gps.is_valid;
            strncpy(g_display.gps_source, gps.source, sizeof(g_display.gps_source) - 1);
            g_display.gps_source[sizeof(g_display.gps_source) - 1] = '\0';
            g_display.latitude_deg = gps.latitude_deg;
            g_display.longitude_deg = gps.longitude_deg;
            g_display.altitude_m = gps.altitude_m;
            g_display.raw_heading = gps.raw_heading;
            g_display.heading_deg = convert_heading_raw_to_degrees(gps.raw_heading);

            strncpy(g_display.cardinal_direction,
                    heading_degrees_to_cardinal(g_display.heading_deg),
                    sizeof(g_display.cardinal_direction) - 1);

            g_display.cardinal_direction[sizeof(g_display.cardinal_direction) - 1] = '\0';

            g_display.raw_speed = gps.raw_speed;
            g_display.satellites_in_use = gps.satellites_in_use;

            /*
            Current lane is not truly known yet.
            We need MAP lane geometry extraction next.
            */
            g_display.current_lane_valid = 0;
            g_display.current_lane_id = -1;
            strcpy(g_display.current_lane_note, "Need MAP lane geometry for true lane detection");
            pthread_mutex_unlock(&g_data_mutex);
        } else {
            fail_count++;

            if (fail_count >= 3) {
                printf("GPS thread: trying to re-login...\n");

                if (obu_login(stok, sizeof(stok))) {
                    printf("GPS thread: re-login successful.\n");
                    fail_count = 0;
                } else {
                    printf("GPS thread: re-login failed.\n");
                }
            }
        }

        usleep(200000);
    }

    return NULL;
}

/*
    ============================================================
    MAP HELPERS
    ============================================================
*/

static void clear_signal_group_map(void)
{
    for(int i = 0; i <= MAX_SIGNAL_GROUPS; ++i) {
        g_signal_group_map[i].lane_count = 0;
        for(int j = 0; j < MAX_LANES_PER_GROUP; ++j) {
            g_signal_group_map[i].lanes[j] = 0;
        }
    }
}

static void add_lane_to_signal_group(int sg, int lane_id)
{
    if(sg < 0 || sg > MAX_SIGNAL_GROUPS) return;

    signal_group_lanes_t* entry = &g_signal_group_map[sg];

    for(int i = 0; i < entry->lane_count; ++i) {
        if(entry->lanes[i] == lane_id) return;
    }

    if(entry->lane_count < MAX_LANES_PER_GROUP) {
        entry->lanes[entry->lane_count++] = lane_id;
    }
}

static double compute_countdown_seconds(int moy, bool moy_valid,
                                        int timeStamp, bool timeStamp_valid,
                                        int minEndTime, bool minEnd_valid)
{
    if(!timeStamp_valid || !minEnd_valid) {
        return -1.0;
    }

    double end_seconds_in_hour = ((double)minEndTime) / 10.0;

    if(moy_valid) {
        int minute_of_hour = moy % 60;
        double current_seconds_in_hour = (minute_of_hour * 60.0) + (((double)timeStamp) / 1000.0);
        double delta = end_seconds_in_hour - current_seconds_in_hour;

        if(delta < 0.0) {
            delta += 3600.0;
        }

        return delta;
    }

    double second_of_minute = ((double)timeStamp) / 1000.0;
    double best_delta = DBL_MAX;

    for(int minute_of_hour = 0; minute_of_hour < 60; ++minute_of_hour) {
        double current_seconds_in_hour = (minute_of_hour * 60.0) + second_of_minute;
        double delta = end_seconds_in_hour - current_seconds_in_hour;

        if(delta < 0.0) {
            delta += 3600.0;
        }

        if(delta < best_delta) {
            best_delta = delta;
        }
    }

    return best_delta;
}

static const char* us_event_state_to_color(long event_state)
{
#ifdef US_MovementPhaseState_stop_And_Remain
    if(event_state == US_MovementPhaseState_stop_And_Remain) return "RED";
#endif
#ifdef US_MovementPhaseState_stop_Then_Proceed
    if(event_state == US_MovementPhaseState_stop_Then_Proceed) return "RED";
#endif
#ifdef US_MovementPhaseState_pre_Movement
    if(event_state == US_MovementPhaseState_pre_Movement) return "RED";
#endif
#ifdef US_MovementPhaseState_permissive_Movement_Allowed
    if(event_state == US_MovementPhaseState_permissive_Movement_Allowed) return "GREEN";
#endif
#ifdef US_MovementPhaseState_protected_Movement_Allowed
    if(event_state == US_MovementPhaseState_protected_Movement_Allowed) return "GREEN";
#endif
#ifdef US_MovementPhaseState_permissive_clearance
    if(event_state == US_MovementPhaseState_permissive_clearance) return "YELLOW";
#endif
#ifdef US_MovementPhaseState_protected_Clearance
    if(event_state == US_MovementPhaseState_protected_Clearance) return "YELLOW";
#endif
#ifdef US_MovementPhaseState_caution_Conflicting_Traffic
    if(event_state == US_MovementPhaseState_caution_Conflicting_Traffic) return "YELLOW";
#endif

    return "UNKNOWN";
}

/*
    ============================================================
    ASN.1 DECODE PIPELINE
    ============================================================
*/

static bool process_uper(cms_fac_msg_type_t type, cms_buffer_view_t msg)
{
    bool error = false;
    void* c_struct = NULL;
    const ASN1CType* asn_type = get_type(type);

    if(NULL == asn_type) {
        error = true;
    }

    if(!error) {
        ASN1Error err;
        int ret = asn1_uper_decode((void**)&c_struct, asn_type, msg.data, msg.length, &err);

        if((ret < 0) || (c_struct == NULL)) {
            error = true;
        }
    }

    if(!error) {
        error = error || l_use_c_stuct(type, c_struct);
        error = error || l_print_xml_format(asn_type, c_struct);
    }

    if(c_struct != NULL) {
        asn1_free_value(asn_type, c_struct);
    }

    return error;
}

static bool l_use_c_stuct(cms_fac_msg_type_t type, void* c_struct)
{
    bool error = false;

    switch(type) {

    case CMS_FAC_MSG_EU_MAP: {
        EU_MAP* c_map = (EU_MAP*)c_struct;

        if(c_map->map.intersections_option) {
            /* EU MAP received, but this program focuses on US MAP/SPaT */
        }

        break;
    }

    case CMS_FAC_MSG_US_MAP: {
        US_MessageFrame* c_message_frame = (US_MessageFrame*)c_struct;

        if(c_message_frame->value.type != asn1_type_US_MapData) {
            error = true;
            break;
        }

        US_MapData* map_data = (US_MapData*)c_message_frame->value.u.data;

        clear_signal_group_map();

        if(map_data->intersections_option) {
            for(size_t i = 0; i < map_data->intersections.count; ++i) {
                US_IntersectionGeometry* inter = &map_data->intersections.tab[i];

                g_last_intersection_id = inter->id.id;

                pthread_mutex_lock(&g_data_mutex);

                g_display.map_valid = 1;
                g_display.intersection_id = inter->id.id;

                /*
                    Intersection name is not added yet because the exact generated
                    field name depends on your ASN.1 headers.
                    We will add it after this compiles.
                */
                if(strlen(g_display.intersection_name) == 0) {
                    strcpy(g_display.intersection_name, "N/A");
                }

                pthread_mutex_unlock(&g_data_mutex);

                for(size_t ln = 0; ln < inter->laneSet.count; ++ln) {
                    US_GenericLane* lane = &inter->laneSet.tab[ln];
                    int lane_id = lane->laneID;

                    if(lane->connectsTo_option) {
                        for(size_t c = 0; c < lane->connectsTo.count; ++c) {
                            US_Connection* conn = &lane->connectsTo.tab[c];

                            if(conn->signalGroup_option) {
                                int sg = conn->signalGroup;
                                add_lane_to_signal_group(sg, lane_id);
                            }
                        }
                    }
                }
            }
        }

        break;
    }

    case CMS_FAC_MSG_US_SPAT: {
        US_MessageFrame* c_message_frame = (US_MessageFrame*)c_struct;

        if(c_message_frame->value.type != asn1_type_US_SPAT) {
            error = true;
            break;
        }

        US_SPAT* spat = (US_SPAT*)c_message_frame->value.u.data;

        pthread_mutex_lock(&g_data_mutex);

        g_display.spat_valid = 1;
        g_display.signal_group_count = 0;

        for(size_t i = 0; i < spat->intersections.count; ++i) {
            US_IntersectionState* inter = &spat->intersections.tab[i];

            int intersection_id = inter->id.id;
            g_display.intersection_id = intersection_id;

            bool moy_valid = false;
            int moy = -1;

            if(inter->moy_option) {
                moy = inter->moy;
                moy_valid = true;
            }

            bool timeStamp_valid = false;
            int timeStamp = -1;

            if(inter->timeStamp_option) {
                timeStamp = inter->timeStamp;
                timeStamp_valid = true;
            }

            for(size_t s = 0; s < inter->states.count; ++s) {
                US_MovementState* ms = &inter->states.tab[s];
                int sg = ms->signalGroup;

                if(ms->state_time_speed.count == 0) {
                    continue;
                }

                US_MovementEvent* ev = &ms->state_time_speed.tab[0];

                long event_state = ev->eventState;
                const char* color = us_event_state_to_color(event_state);

                bool minEnd_valid = false;
                int minEndTime = -1;

                if(ev->timing_option) {
                    minEndTime = ev->timing.minEndTime;
                    minEnd_valid = true;
                }

                double countdown = compute_countdown_seconds(
                    moy, moy_valid,
                    timeStamp, timeStamp_valid,
                    minEndTime, minEnd_valid
                );

                int idx = g_display.signal_group_count;

                if(idx < MAX_DISPLAY_SIGNALS) {
                    g_display.signal_groups[idx] = sg;

                    strncpy(g_display.signal_colors[idx], color, sizeof(g_display.signal_colors[idx]) - 1);
                    g_display.signal_colors[idx][sizeof(g_display.signal_colors[idx]) - 1] = '\0';

                    g_display.signal_countdowns[idx] = countdown;

                    g_display.lane_count[idx] = 0;

                    if(sg >= 0 && sg <= MAX_SIGNAL_GROUPS) {
                        int count = g_signal_group_map[sg].lane_count;

                        for(int k = 0; k < count && k < MAX_DISPLAY_LANES; ++k) {
                            g_display.lanes[idx][k] = g_signal_group_map[sg].lanes[k];
                            g_display.lane_count[idx]++;
                        }
                    }

                    g_display.signal_group_count++;
                }
            }
        }

        pthread_mutex_unlock(&g_data_mutex);

        break;
    }

    default:
        break;
    }

    return error;
}

static bool l_print_xml_format(const ASN1CType* asn_type, void* c_struct)
{
    uint8_t* xer_ptr = NULL;

    int ret = asn1_xer_encode((uint8_t**)&xer_ptr, asn_type, c_struct);

    if((ret < 0) || (xer_ptr == NULL)) {
        return true;
    }

    FILE *log_file = fopen(filename, "a");

    if(log_file == NULL) {
        asn1_free(xer_ptr);
        return true;
    }

    fputs((char*)xer_ptr, log_file);

    size_t n = strlen((char*)xer_ptr);

    if(n == 0 || ((char*)xer_ptr)[n - 1] != '\n') {
        fputc('\n', log_file);
    }

    fflush(log_file);
    fclose(log_file);

    asn1_free(xer_ptr);
    return false;
}

/*
    ============================================================
    FACILITY MESSAGE CALLBACK
    ============================================================
*/

static void fac_notif_cb(cms_fac_msg_type_t type,
                         const cms_fac_notif_data_t* notif,
                         cms_buffer_view_t msg,
                         void* ctx)
{
    if((NULL == notif) || (NULL == msg.data) || (0UL == msg.length) || (NULL == ctx)) {
        return;
    }

    notif_ctx_t* notif_ctx = (notif_ctx_t*)ctx;

    pthread_mutex_lock(&g_data_mutex);

    g_display.callback_count++;
    g_display.last_msg_type = type;
    g_display.last_msg_length = msg.length;

    pthread_mutex_unlock(&g_data_mutex);
    
    process_uper(type, msg);

    ++notif_ctx->cnt;
}

static const char* get_verify_result_string(cms_sec_verify_result_t verify_result_code)
{
    switch(verify_result_code) {
    case CMS_SEC_VERIFY_UNSECURED:
        return "Unsecured - No signature";
    case CMS_SEC_VERIFY_VERIFIED:
        return "Verified";
    default:
        return "Verification Failed";
    }
}

static const ASN1CType* get_type(cms_fac_msg_type_t in_type)
{
    const ASN1CType* asn_type = NULL;

    for(int i = 0; asn_types[i].asn_type != NULL; ++i) {
        if(in_type == asn_types[i].cms_type) {
            asn_type = asn_types[i].asn_type;
            break;
        }
    }

    return asn_type;
}

/*
    ============================================================
    MAP/SPAT THREAD

    This is your original main logic moved into a thread.
    It connects to the OBU and waits for facility messages.
    ============================================================
*/

void* spat_map_thread_func(void* arg)
{
    const char* host = (const char*)arg;

    cms_session_t session = cms_get_session();
    bool error = cms_api_connect_easy(&session, host);

    notif_ctx_t wildcard_ctx = {
        .param = 2U,
        .cnt = 0U
    };

    cms_subs_id_t wildcard_subs_id = CMS_SUBS_ID_INVALID;

    if(error) {
        printf("MAP/SPaT thread: unable to connect to OBU SDK at %s\n", host);
        return NULL;
    }

    error = error || cms_fac_subscribe(&session,
                                       CMS_FAC_SUBSCRIBE_ALL,
                                       &fac_notif_cb,
                                       &wildcard_ctx,
                                       &wildcard_subs_id);

    if(error) {
        printf("MAP/SPaT thread: unable to subscribe to facility notifications.\n");
    } else {
        printf("MAP/SPaT thread: subscribed successfully.\n");

        while(g_running) {
            sleep(1);
        }
    }

    cms_fac_unsubscribe(&session, wildcard_subs_id);
    cms_api_disconnect(&session);
    cms_api_clean();

    return NULL;
}

/*
    ============================================================
    TERMINAL DISPLAY
    ============================================================
*/

void print_terminal_display(void)
{
    SharedDisplayData copy;

    pthread_mutex_lock(&g_data_mutex);
    copy = g_display;
    pthread_mutex_unlock(&g_data_mutex);

    printf("\033[H\033[J");

    printf("============================================================\n");
    printf("                 V2X TERMINAL DISPLAY\n");
    printf("============================================================\n\n");

    printf("GPS\n");
    printf("Valid:              %s\n", copy.gps_valid ? "true" : "false");
    printf("Source:             %s\n", strlen(copy.gps_source) > 0 ? copy.gps_source : "N/A");
    printf("Latitude:           %.7f\n", copy.latitude_deg);
    printf("Longitude:          %.7f\n", copy.longitude_deg);
    printf("Altitude estimate:  %.2f m\n", copy.altitude_m);
    printf("Heading raw:        %lld\n", copy.raw_heading);
    printf("Heading degrees:    %.2f deg\n", copy.heading_deg);
    printf("Direction:          %s\n", copy.cardinal_direction);

    printf("Speed raw:          %lld\n", copy.raw_speed);
    printf("Satellites:         %lld\n\n", copy.satellites_in_use);

    printf("CURRENT VEHICLE POSITION\n");

    if(copy.current_lane_valid) {
        printf("Current Lane:       %d\n", copy.current_lane_id);
    } else {
        printf("Current Lane:       N/A\n");
    }

    printf("Lane Status:        %s\n\n", copy.current_lane_note);
    printf("MAP\n");
    printf("Map Valid:          %s\n", copy.map_valid ? "true" : "false");
    printf("Intersection ID:    %d\n", copy.intersection_id);
    printf("Intersection Name:  %s\n\n",
           strlen(copy.intersection_name) > 0 ? copy.intersection_name : "N/A");
    printf("Callback Count:     %u\n", copy.callback_count);
    printf("Last Msg Type:      %d\n", copy.last_msg_type);
    printf("Last Msg Length:    %u\n", copy.last_msg_length);

    printf("SPaT\n");

    if(!copy.spat_valid) {
        printf("Waiting for SPaT messages...\n");
    } else {
        for(int i = 0; i < copy.signal_group_count; ++i) {
            printf("SG %2d | color=%-7s | countdown=",
                   copy.signal_groups[i],
                   copy.signal_colors[i]);

            if(copy.signal_countdowns[i] >= 0.0) {
                printf("%5.1fs", copy.signal_countdowns[i]);
            } else {
                printf("  N/A ");
            }

            printf(" | lanes=[");

            for(int j = 0; j < copy.lane_count[i]; ++j) {
                printf("%d", copy.lanes[i][j]);

                if(j < copy.lane_count[i] - 1) {
                    printf(", ");
                }
            }

            printf("]\n");
        }
    }

    printf("\n============================================================\n");
    printf("Press Ctrl + C to stop.\n");
}

/*
    ============================================================
    MAIN
    ============================================================
*/

int main(int argc, char* argv[])
{
    const char* host = (argc > 1) ? argv[1] : OBU_IP;

    pthread_t gps_thread;
    pthread_t spat_map_thread;

    memset(&g_display, 0, sizeof(g_display));

    strcpy(g_display.gps_source, "N/A");
    strcpy(g_display.cardinal_direction, "UNKNOWN");

    g_display.current_lane_valid = 0;
    g_display.current_lane_id = -1;
    strcpy(g_display.current_lane_note, "Waiting for lane match");

    strcpy(g_display.intersection_name, "N/A");
    g_display.intersection_id = -1;

    FILE *reset_log = fopen(filename, "w");
    if(reset_log != NULL) {
        fclose(reset_log);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("Starting V2X terminal display...\n");
    printf("OBU host: %s\n", host);

    pthread_create(&gps_thread, NULL, gps_thread_func, NULL);
    pthread_create(&spat_map_thread, NULL, spat_map_thread_func, (void*)host);

    usleep(500000);

    while(1) {
        print_terminal_display();
        usleep(200000);
    }

    g_running = 0;

    pthread_join(gps_thread, NULL);
    pthread_join(spat_map_thread, NULL);

    curl_global_cleanup();

    return 0;
}
