/*
 * Purpose of this program:
 * ------------------------
 * This C file connects to a Commsignia V2X unit, subscribes to incoming
 * facility-layer messages, decodes the ASN.1 UPER payloads, and prints useful
 * traffic-signal information to the terminal.
 *
 * The most important messages for this project:
 *   - MAP:  describes the intersection geometry, lanes, and which signal group
 *           controls each lane.
 *   - SPaT: describes the live Signal Phase and Timing state, meaning the
 *           current light color/state and the predicted end time for that state.
 *
 * MAP message tells the program "which lanes belong to
 * which signal groups," while the SPaT message tells the program "what each
 * signal group is currently doing and when it may change."
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <float.h>

#include <cms_v2x/api.h>
#include <cms_v2x/fac_types.h>
#include <cms_v2x/fac_subscribe.h>

#include <asn1/v2x_eu_asn.h>
#include <asn1/v2x_us_asn.h>
#include <asn1defs.h>

enum { ASN_NAME_LENGTH = 64U }; 
static const char *filename = "log.txt"; //// this is the file where decoded XML messages are saved 

/** @file
@brief Subscribe to facility messages
@ingroup ex
*/

/*
 * Associates a Commsignia facility message type with the ASN.1 type needed to
 * decode it. The Commsignia callback only tells us the general message type;
 * this table lets us choose the correct generated ASN.1 decoder
 */
typedef struct {
    cms_fac_msg_type_t cms_type;
    char name[ASN_NAME_LENGTH];
    const ASN1CType* asn_type;
} asn_type_t;

/*
 * This table lists the message types this program knows how to recognize
 * When a message arrives, the program checks this table to know how to open it
 */
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
    { .cms_type = CMS_FAC_MSG_US_MAP,  .name = "US_MAP",  .asn_type = asn1_type_US_MessageFrame }, //Map data
    { .cms_type = CMS_FAC_MSG_US_SPAT, .name = "US_SPAT", .asn_type = asn1_type_US_MessageFrame }, //Spat data
    { .cms_type = CMS_FAC_MSG_US_SRM,  .name = "US_SRM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_SSM,  .name = "US_SSM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_PSM,  .name = "US_PSM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_PVD,  .name = "US_PVD",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_PDM,  .name = "US_PDM",  .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_RTCM, .name = "US_RTCM", .asn_type = asn1_type_US_MessageFrame },
    { .cms_type = CMS_FAC_MSG_US_WSA,  .name = "US_WSA",  .asn_type = asn1_type_US_MessageFrame },

    /* Terminator */
    { .name = "", .asn_type = NULL }
};

/* Context type for the notification callback */
typedef struct notif_ctx {
    uint32_t param;
    uint32_t cnt;
} notif_ctx_t;

/*
 * This section remembers which lanes belong to each traffic-light group
 *
 * Example:
 *   Signal Group 5 -> lanes 2 and 3
 *
 * Later, if SPaT says "Signal Group 5 is green," the program can also show
 * that lanes 2 and 3 are the lanes affected by that green light
 */
#define MAX_SIGNAL_GROUPS   64
#define MAX_LANES_PER_GROUP 32

typedef struct {
    int lane_count; // Number of lane numbers saved here
    int lanes[MAX_LANES_PER_GROUP]; // The lane numbers saved for this signal group
} signal_group_lanes_t;

static signal_group_lanes_t g_signal_group_map[MAX_SIGNAL_GROUPS + 1];
static int g_last_intersection_id = -1;

/*
 * These are function previews.
 * They tell our C script that these functions exist before the full functions appear below
 */
static const char* get_verify_result_string(cms_sec_verify_result_t verify_result_code);
static const ASN1CType* get_type(cms_fac_msg_type_t in_type);
static bool process_uper(cms_fac_msg_type_t type, cms_buffer_view_t msg);
static bool l_use_c_stuct(cms_fac_msg_type_t type, void* c_struct);
static bool l_print_xml_format(const ASN1CType* asn_type, void* c_struct);

/*
 * Helper functions
 * These are small functions that make the main code easier to read
 */
/*
 * Clear the saved lane information
 * This is done when a new MAP message arrives, so old lane data is removed
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
/*
 * Save one lane under one signal group
 * If the lane is already saved, no add it again
 */
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

//Print the lanes that belong to a signal group

static void print_lanes_for_signal_group(int sg)
{
    if(sg < 0 || sg > MAX_SIGNAL_GROUPS) return;
    if(g_signal_group_map[sg].lane_count <= 0) return;

    printf(" | lanes=[");
    for(int i = 0; i < g_signal_group_map[sg].lane_count; ++i) {
        printf("%d", g_signal_group_map[sg].lanes[i]);
        if(i < g_signal_group_map[sg].lane_count - 1) {
            printf(", ");
        }
    }
    printf("]");
}
/*
 * Estimate how many seconds are left before the light changes
 * If the needed timing information is missing, return -1.0 to mean "not available"
 */
static double compute_countdown_seconds(int moy, bool moy_valid,
                                        int timeStamp, bool timeStamp_valid,
                                        int minEndTime, bool minEnd_valid)
{
    if(!timeStamp_valid || !minEnd_valid) {
        return -1.0;
    }

    //Change the light-change time into seconds
    double end_seconds_in_hour = ((double)minEndTime) / 10.0;

    //If the message gives us enough time information, use it directly
    if(moy_valid) {
        int minute_of_hour = moy % 60;
        double current_seconds_in_hour = (minute_of_hour * 60.0) + (((double)timeStamp) / 1000.0);
        double delta = end_seconds_in_hour - current_seconds_in_hour;

        if(delta < 0.0) {
            delta += 3600.0;
        }
        return delta;
    }

    //If some time information is missing, try the best reasonable estimate
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
    //Convert the traffic-light state into a simple word: RED, YELLOW, or GREEN
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
 * Message decoding section
 * This is where the program turns a raw incoming message into readable data
 */
/*
 * Decode one incoming message
 * The message starts as raw data, and this function turns it into a C structure
 * that the rest of the program can read
 */
static bool process_uper(cms_fac_msg_type_t type, cms_buffer_view_t msg)
{
    bool error = false;
    void* c_struct = NULL;
    const ASN1CType* asn_type = get_type(type);

    if(NULL == asn_type) {
        error = true;
        fprintf(stderr, "Unable to detect ASN type\n");
    }

    if(!error) {
        ASN1Error err;
        //Translate the raw V2X message into readable C data
        int ret = asn1_uper_decode((void**)&c_struct, asn_type, msg.data, msg.length, &err);
        if((ret < 0) || (c_struct == NULL)) {
            fprintf(stderr, "Decoding error: %s\n", err.msg);
            error = true;
        }
    }

    if(!error) {
        //Print the useful information, then save the full message to the log file.
        error = error || l_use_c_stuct(type, c_struct);
        error = error || l_print_xml_format(asn_type, c_struct);
    }

    if(c_struct != NULL) {
        //Free the memory used by the decoded message
        asn1_free_value(asn_type, c_struct);
    }

    return error;
}
/*
 * If it is MAP, save lane information.
 * If it is SPaT, print the live traffic-light information.
 */

static bool l_use_c_stuct(cms_fac_msg_type_t type, void* c_struct)
{
    bool error = false;

    switch(type) {

    case CMS_FAC_MSG_EU_MAP: {
        //MAP message: learn the intersection layout and lane-to-signal-group links
        EU_MAP* c_map = (EU_MAP*)c_struct;
        if(c_map->map.intersections_option) {
            fprintf(stderr, "EU MAP intersections: %zu\n", c_map->map.intersections.count);
        }
        break;
    }

    case CMS_FAC_MSG_US_MAP: {
        US_MessageFrame* c_message_frame = (US_MessageFrame*)c_struct;

        if(c_message_frame->value.type != asn1_type_US_MapData) {
            fprintf(stderr, "Error: message is not US MapData\n");
            error = true;
            break;
        }

        US_MapData* map_data = (US_MapData*)c_message_frame->value.u.data;
        //A new MAP message arrived, so remove the old saved lane map first
        clear_signal_group_map();

        if(map_data->intersections_option) {
            fprintf(stderr, "US MAP intersections: %zu\n", map_data->intersections.count);
            //Go through every intersection in the MAP message
            for(size_t i = 0; i < map_data->intersections.count; ++i) {
                US_IntersectionGeometry* inter = &map_data->intersections.tab[i];;
                g_last_intersection_id = inter->id.id;
                //Go through every lane in this intersection
                for(size_t ln = 0; ln < inter->laneSet.count; ++ln) {
                    US_GenericLane* lane = &inter->laneSet.tab[ln];
                    int lane_id = lane->laneID;
                    //If this lane is connected to a traffic-light group, save that connection
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

            printf("============================================================\n");
            printf("MAP LOADED\n");
            printf("============================================================\n");
            printf("Intersection ID: %d\n", g_last_intersection_id);
            for(int sg = 0; sg <= MAX_SIGNAL_GROUPS; ++sg) {
                if(g_signal_group_map[sg].lane_count > 0) {
                    printf("SG %2d -> lanes=[", sg);
                    for(int i = 0; i < g_signal_group_map[sg].lane_count; ++i) {
                        printf("%d", g_signal_group_map[sg].lanes[i]);
                        if(i < g_signal_group_map[sg].lane_count - 1) {
                            printf(", ");
                        }
                    }
                    printf("]\n");
                }
            }
        }
        break;
    }

    case CMS_FAC_MSG_US_SPAT: {
        //SPaT message: show what the lights are doing right now
        US_MessageFrame* c_message_frame = (US_MessageFrame*)c_struct;

        if(c_message_frame->value.type != asn1_type_US_SPAT) {
            fprintf(stderr, "Error: message is not US SPAT\n");
            error = true;
            break;
        }

        
        US_SPAT* spat = (US_SPAT*)c_message_frame->value.u.data;

        printf("============================================================\n");
        printf("LIVE SPAT COUNTDOWN\n");
        printf("============================================================\n");
        //Go through each intersection in the SPaT message.
        for(size_t i = 0; i < spat->intersections.count; ++i) {
            US_IntersectionState* inter = &spat->intersections.tab[i];

            int intersection_id = inter->id.id;

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

            printf("Intersection %d | moy=%s | timeStamp=%s\n",
                   intersection_id,
                   moy_valid ? "present" : "none",
                   timeStamp_valid ? "present" : "none");
            printf("------------------------------------------------------------\n");
            //Go through each signal group, which is a traffic-light group    
            for(size_t s = 0; s < inter->states.count; ++s) {
                US_MovementState* ms = &inter->states.tab[s];
                int sg = ms->signalGroup;

                if(ms->state_time_speed.count == 0) {
                    continue;
                }

                US_MovementEvent* ev = &ms->state_time_speed.tab[0];

                long event_state = ev->eventState;
                //Convert the official light state into RED, YELLOW, GREEN, or UNKNOWN
                const char* color = us_event_state_to_color(event_state);

                bool minEnd_valid = false;
                int minEndTime = -1;

                bool maxEnd_valid = false;
                int maxEndTime = -1;

                if(ev->timing_option) {
                   minEndTime = ev->timing.minEndTime;
                   minEnd_valid = true;

                   if(ev->timing.maxEndTime_option) {
                      maxEndTime = ev->timing.maxEndTime;
                      maxEnd_valid = true;
    }
}
                //calculate how many seconds are left before the light changes
                double countdown = compute_countdown_seconds(
                    moy, moy_valid,
                    timeStamp, timeStamp_valid,
                    minEndTime, minEnd_valid
                );
                //Print one easy-to-read line for this signal group
                printf("SG %2d | color=%-6s | minEnd=", sg, color);
                if(minEnd_valid) printf("%6d", minEndTime);
                else printf("   N/A");

                printf(" | maxEnd=");
                if(maxEnd_valid) printf("%6d", maxEndTime);
                else printf("   N/A");

                printf(" | countdown=");
                if(countdown >= 0.0) printf("%5.1fs", countdown);
                else printf("  N/A");

                print_lanes_for_signal_group(sg);
                printf("\n");
            }
        }
        break;
    }

    default:
        break;
    }

    return error;
}
/*
 * Save the full decoded message into log.txt.
 * This is useful because terminal only shows the most important parts,
 * but the log file keeps the full messages in XML format.
 */
static bool l_print_xml_format(const ASN1CType* asn_type, void* c_struct)
{
    uint8_t* xer_ptr = NULL;

    //Convert the decoded message into XML text
    int ret = asn1_xer_encode((uint8_t**)&xer_ptr, asn_type, c_struct);
    if((ret < 0) || (xer_ptr == NULL)) {
        return true;
    }
    //Open the log file in append mode so new messages are added to the end
    FILE *log_file = fopen(filename, "a");
    if(log_file == NULL) {
        asn1_free(xer_ptr);
        return true;
    }
    //Write the full XML message into the log file
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

//This function runs automatically every time a new V2X message arrives.
static void fac_notif_cb(cms_fac_msg_type_t type,
                         const cms_fac_notif_data_t* notif,
                         cms_buffer_view_t msg,
                         void* ctx)
{
    //If something important is missing, stop before the program crashes
    if((NULL == notif) || (NULL == msg.data) || (0UL == msg.length) || (NULL == ctx)) {
        fprintf(stderr, "%s NULL argument\n", __func__);
        return;
    }

    notif_ctx_t* notif_ctx = (notif_ctx_t*)ctx;

    fprintf(stderr, "Context: %lu\n", (unsigned long)notif_ctx->param);
    fprintf(stderr, "Receive counter: %lu\n", (unsigned long)notif_ctx->cnt);
    fprintf(stderr, "Type: %d\n", type);
    fprintf(stderr, "Timestamp: %llu [ms]\n",
            (unsigned long long)notif->radio.timestamp);
    fprintf(stderr, "Source address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            notif->radio.source_address[0],
            notif->radio.source_address[1],
            notif->radio.source_address[2],
            notif->radio.source_address[3],
            notif->radio.source_address[4],
            notif->radio.source_address[5]);
    fprintf(stderr, "Verification result: %s (%lu)\n",
            get_verify_result_string(notif->security.verify_result),
            (unsigned long)notif->security.verify_result);
    //Decode and handle the actual V2X message  
    process_uper(type, msg);

    fprintf(stderr, "=====================================================\n");
    //Count this message so the program knows how many it has received
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
            fprintf(stderr, "type detected: %s\n", asn_types[i].name);
            asn_type = asn_types[i].asn_type;
            break;
        }
    }

    return asn_type;
}

/*
 * Program starts here
 * It connects to the Commsignia device, waits for messages, and cleans up
 * when program is done
 */
int main(int argc, char* argv[])
{
    //Use the IP address typed by the user, or use the default IP address
    const char* host = (argc > 1) ? argv[1] : "192.168.1.54";

    //Start with an empty log file each time the program runs
    FILE *reset_log = fopen(filename, "w");
    if(reset_log != NULL) {
        fclose(reset_log);
    }
    //Start a Commsignia session and connect to the device
    cms_session_t session = cms_get_session();
    bool error = cms_api_connect_easy(&session, host);

    notif_ctx_t wildcard_ctx = { 
        .param = 2U,
        .cnt = 0U
    };
    //Subscribe so the program starts receiving V2X messages
    cms_subs_id_t wildcard_subs_id = CMS_SUBS_ID_INVALID;
    error = error || cms_fac_subscribe(&session,
                                       CMS_FAC_SUBSCRIBE_ALL,
                                       &fac_notif_cb,
                                       &wildcard_ctx,
                                       &wildcard_subs_id);

    if(error) {
        fprintf(stderr, "Unable to subscribe to all facility notifications\n");
    } else {
        static const uint32_t EXIT_ON_RECV_COUNT = 20000UL;
        //Keep the program alive while it waits for messages
        while(wildcard_ctx.cnt < EXIT_ON_RECV_COUNT) {
            fprintf(stderr, "Waiting for facility messages\n");
            sleep(10);
        }
    }
    //Stop listening and disconnect from the device
    error = error || cms_fac_unsubscribe(&session, wildcard_subs_id);
    cms_api_disconnect(&session);
    cms_api_clean();

    return (int)error;
}