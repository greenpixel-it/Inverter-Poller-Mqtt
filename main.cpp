// Lightweight program to take the sensor data from a Voltronic Axpert, Mppsolar PIP, Voltacon, Effekta, and other branded OEM Inverters and send it to a MQTT server for ingestion...
// Adapted from "Maio's" C application here: https://skyboo.net/2017/03/monitoring-voltronic-power-axpert-mex-inverter-under-linux/
//
// Please feel free to adapt this code and add more parameters -- See the following forum for a breakdown on the RS323 protocol: http://forums.aeva.asn.au/viewtopic.php?t=4332
// ------------------------------------------------------------------------

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <sys/file.h>

#include "main.h"
#include "tools.h"
#include "inputparser.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

#include <mosquitto.h>

bool debugFlag = false;
bool runOnce = false;
bool ups_leave = false;

cInverter *ups = NULL;

atomic_bool ups_status_changed(false);
atomic_bool ups_qmod_changed(false);
atomic_bool ups_qpiri_changed(false);
atomic_bool ups_qpigs_changed(false);
atomic_bool ups_qpiws_changed(false);
atomic_bool ups_cmd_executed(false);


// ---------------------------------------
// Global configs read from 'inverter.conf'

string devicename;
int runinterval;
float ampfactor;
float wattfactor;
int qpiri = 98;
int qpiws = 36;
int qmod = 5;
int qpigs = 110;

string mqtt_server;
int mqtt_port = 1883 ;
string mqtt_topic;
string mqtt_devicename;
string mqtt_username;
string mqtt_password;

// ---------------------------------------

void attemptAddSetting(int *addTo, string addFrom) {
    try {
        *addTo = stof(addFrom);
    } catch (exception e) {
        cout << e.what() << '\n';
        cout << "There's probably a string in the settings file where an int should be.\n";
    }
}

void attemptAddSetting(float *addTo, string addFrom) {
    try {
        *addTo = stof(addFrom);
    } catch (exception e) {
        cout << e.what() << '\n';
        cout << "There's probably a string in the settings file where a floating point should be.\n";
    }
}

void getSettingsFile(string filename) {

    try {
        string fileline, linepart1, linepart2;
        ifstream infile;
        infile.open(filename);
        while(!infile.eof()) {
            getline(infile, fileline);
            size_t firstpos = fileline.find("#");

            if(firstpos != 0 && fileline.length() != 0) {    // Ignore lines starting with # (comment lines)
                size_t delimiter = fileline.find("=");
                linepart1 = fileline.substr(0, delimiter);
                linepart2 = fileline.substr(delimiter+1, string::npos - delimiter);
                if(linepart1 == "device")
                    devicename = linepart2;
                else if(linepart1 == "run_interval")
                    attemptAddSetting(&runinterval, linepart2);
                else if(linepart1 == "amperage_factor")
                    attemptAddSetting(&ampfactor, linepart2);
                else if(linepart1 == "watt_factor")
                    attemptAddSetting(&wattfactor, linepart2);
                else if(linepart1 == "qpiri")
                    attemptAddSetting(&qpiri, linepart2);
                else if(linepart1 == "qpiws")
                    attemptAddSetting(&qpiws, linepart2);
                else if(linepart1 == "qmod")
                    attemptAddSetting(&qmod, linepart2);
                else if(linepart1 == "qpigs")
                    attemptAddSetting(&qpigs, linepart2);
                else if(linepart1 == "server")
                    mqtt_server = linepart2;
                else if(linepart1 == "port")
                    attemptAddSetting(&mqtt_port, linepart2);
                else if(linepart1 == "topic")
                    mqtt_topic = linepart2;
                else if(linepart1 == "devicename")
                    mqtt_devicename = linepart2;
                else if(linepart1 == "username")
                    mqtt_username = linepart2;
                else if(linepart1 == "password")
                    mqtt_password = linepart2;
                else
                    continue;
            }
        }
        infile.close();
    } catch (...) {
        cout << "Settings could not be read properly...\n";
    }
}

int main(int argc, char* argv[]) {

    // Reply1
    float voltage_grid;
    float freq_grid;
    float voltage_out;
    float freq_out;
    int load_va;
    int load_watt;
    int load_percent;
    int voltage_bus;
    float voltage_batt;
    int batt_charge_current;
    int batt_capacity;
    int temp_heatsink;
    float pv_input_current;
    float pv_input_voltage;
    float pv_input_watts;
    //float pv_input_watthour;
    //float load_watthour = 0;
    float scc_voltage;
    int batt_discharge_current;
    char device_status[9];

    // Reply2
    float grid_voltage_rating;
    float grid_current_rating;
    float out_voltage_rating;
    float out_freq_rating;
    float out_current_rating;
    int out_va_rating;
    int out_watt_rating;
    float batt_rating;
    float batt_recharge_voltage;
    float batt_under_voltage;
    float batt_bulk_voltage;
    float batt_float_voltage;
    int batt_type;
    int max_grid_charge_current;
    int max_charge_current;
    int in_voltage_range;
    int out_source_priority;
    int charger_source_priority;
    int machine_type;
    int topology;
    int out_mode;
    float batt_redischarge_voltage;

    // Get command flag settings from the arguments (if any)
    InputParser cmdArgs(argc, argv);
    const string &rawcmd = cmdArgs.getCmdOption("-r");
    int replylen = 7;
    sscanf(cmdArgs.getCmdOption("-l").c_str(), "%d", &replylen);

    if(cmdArgs.cmdOptionExists("-h") || cmdArgs.cmdOptionExists("--help")) {
        return print_help();
    }
    if(cmdArgs.cmdOptionExists("-d")) {
        debugFlag = true;
    }
    if(cmdArgs.cmdOptionExists("-1") || cmdArgs.cmdOptionExists("--run-once")) {
        runOnce = true;
    }
    lprintf("INVERTER: Debug set");
    const char *settings;

    // Get the rest of the settings from the conf file
    if( access( "/etc/inverter/inverter.conf", F_OK ) != -1 ) { // file exists
        settings = "/etc/inverter/inverter.conf";
    } else { // file doesn't exist
        settings = "../inverter.conf";
    }
    getSettingsFile(settings);
    int fd = open(settings, O_RDWR);
    while (flock(fd, LOCK_EX)) sleep(1);

    // #### MQTT ADDON ####
    struct mosquitto *mosq;
    int rc;

    // Mosquitto Initialization
    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        printf("Error in the client initialization\n");
        return 1;
    }

    // set of username and password
    rc = mosquitto_username_pw_set(mosq, mqtt_username.c_str(), mqtt_password.c_str());
    if (rc != MOSQ_ERR_SUCCESS) {
        printf("Error in credential settings: %s\n", mosquitto_strerror(rc));
        return 1;
    }

    // broker connection
    rc = mosquitto_connect(mosq, mqtt_server.c_str(), mqtt_port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        printf("Failed connecting to the broker %s\n", mosquitto_strerror(rc));
        return 1;
    }

    bool ups_status_changed(false);
    ups = new cInverter(devicename);

    // Logic to send 'raw commands' to the inverter..
    if (!rawcmd.empty()) {
        ups->ExecuteCmd(rawcmd);
        // We're piggybacking off the qpri status response...
        printf("Reply:  %s\n", ups->GetQpiriStatus()->c_str());
        exit(0);
    } else {
        ups->runMultiThread();
    }

    while (true) {
        if (ups_status_changed) {
            int mode = ups->GetMode();

            if (mode)
                lprintf("INVERTER: Mode Currently set to: %d", mode);

            ups_status_changed = false;
        }

        if (ups_qmod_changed && ups_qpiri_changed && ups_qpigs_changed) {

            ups_qmod_changed = false;
            ups_qpiri_changed = false;
            ups_qpigs_changed = false;

            int mode = ups->GetMode();
            char mode_raw = ups->GetModeRaw();
            string *reply1   = ups->GetQpigsStatus();
            string *reply2   = ups->GetQpiriStatus();
            string *warnings = ups->GetWarnings();

            if (reply1 && reply2 && warnings) {

                // Parse and display values
                sscanf(reply1->c_str(), "%f %f %f %f %d %d %d %d %f %d %d %d %f %f %f %d %s", &voltage_grid, &freq_grid, &voltage_out, &freq_out, &load_va, &load_watt, &load_percent, &voltage_bus, &voltage_batt, &batt_charge_current, &batt_capacity, &temp_heatsink, &pv_input_current, &pv_input_voltage, &scc_voltage, &batt_discharge_current, &device_status);
                sscanf(reply2->c_str(), "%f %f %f %f %f %d %d %f %f %f %f %f %d %d %d %d %d %d %d %d %d %f", &grid_voltage_rating, &grid_current_rating, &out_voltage_rating, &out_freq_rating, &out_current_rating, &out_va_rating, &out_watt_rating, &batt_rating, &batt_recharge_voltage, &batt_under_voltage, &batt_bulk_voltage, &batt_float_voltage, &batt_type, &max_grid_charge_current, &max_charge_current, &in_voltage_range, &out_source_priority, &charger_source_priority, &machine_type, &topology, &out_mode, &batt_redischarge_voltage);

                // There appears to be a discrepancy in actual DMM measured current vs what the meter is
                // telling me it's getting, so lets add a variable we can multiply/divide by to adjust if
                // needed.  This should be set in the config so it can be changed without program recompile.
                if (debugFlag) {
                    printf("INVERTER: ampfactor from config is %.2f\n", ampfactor);
                    printf("INVERTER: wattfactor from config is %.2f\n", wattfactor);
                }

                pv_input_current = pv_input_current * ampfactor;

                // It appears on further inspection of the documentation, that the input current is actually
                // current that is going out to the battery at battery voltage (NOT at PV voltage).  This
                // would explain the larger discrepancy we saw before.

                pv_input_watts = (scc_voltage * pv_input_current) * wattfactor;

                // Calculate watt-hours generated per run interval period (given as program argument)
                //pv_input_watthour = pv_input_watts / (3600 / runinterval);
                //load_watthour = (float)load_watt / (3600 / runinterval);

                // Print as JSON (output is expected to be parsed by another tool...)
                /*
                printf("{\n");
                printf("  \"Inverter_mode\":%d,\n", mode);
                printf("  \"AC_grid_voltage\":%.1f,\n", voltage_grid);
                printf("  \"AC_grid_frequency\":%.1f,\n", freq_grid);
                printf("  \"AC_out_voltage\":%.1f,\n", voltage_out);
                printf("  \"AC_out_frequency\":%.1f,\n", freq_out);
                printf("  \"PV_in_voltage\":%.1f,\n", pv_input_voltage);
                printf("  \"PV_in_current\":%.1f,\n", pv_input_current);
                printf("  \"PV_in_watts\":%.1f,\n", pv_input_watts);
                printf("  \"PV_in_watthour\":%.4f,\n", pv_input_watthour);
                printf("  \"SCC_voltage\":%.4f,\n", scc_voltage);
                printf("  \"Load_pct\":%d,\n", load_percent);
                printf("  \"Load_watt\":%d,\n", load_watt);
                printf("  \"Load_watthour\":%.4f,\n", load_watthour);
                printf("  \"Load_va\":%d,\n", load_va);
                printf("  \"Bus_voltage\":%d,\n", voltage_bus);
                printf("  \"Heatsink_temperature\":%d,\n", temp_heatsink);
                printf("  \"Battery_capacity\":%d,\n", batt_capacity);
                printf("  \"Battery_voltage\":%.2f,\n", voltage_batt);
                printf("  \"Battery_charge_current\":%d,\n", batt_charge_current);
                printf("  \"Battery_discharge_current\":%d,\n", batt_discharge_current);
                printf("  \"Load_status_on\":%c,\n", device_status[3]);
                printf("  \"SCC_charge_on\":%c,\n", device_status[6]);
                printf("  \"AC_charge_on\":%c,\n", device_status[7]);
                printf("  \"Battery_recharge_voltage\":%.1f,\n", batt_recharge_voltage);
                printf("  \"Battery_under_voltage\":%.1f,\n", batt_under_voltage);
                printf("  \"Battery_bulk_voltage\":%.1f,\n", batt_bulk_voltage);
                printf("  \"Battery_float_voltage\":%.1f,\n", batt_float_voltage);
                printf("  \"Max_grid_charge_current\":%d,\n", max_grid_charge_current);
                printf("  \"Max_charge_current\":%d,\n", max_charge_current);
                printf("  \"Out_source_priority\":%d,\n", out_source_priority);
                printf("  \"Charger_source_priority\":%d,\n", charger_source_priority);
                printf("  \"Battery_redischarge_voltage\":%.1f,\n", batt_redischarge_voltage);
                printf("  \"Warnings\":\"%s\"\n", warnings->c_str());
                printf("}\n");
                */

                char print_str[50]; // Array piu grande per garantire spazio sufficiente
                sprintf(print_str, "%d", mode);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Inverter_mode").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%c", mode_raw);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Inverter_mode_raw").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", voltage_grid);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_AC_grid_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", freq_grid);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_AC_grid_frequency").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", voltage_out);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_AC_out_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", freq_out);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_AC_out_frequency").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", pv_input_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_PV_in_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", pv_input_current);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_PV_in_current").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", pv_input_watts);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_PV_in_watts").c_str(), strlen(print_str), print_str, 0, false);
                //sprintf(print_str, "%.4f", pv_input_watthour);
                //mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_PV_in_watthour").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.4f", scc_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_SCC_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", load_percent);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Load_pct").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", load_watt);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Load_watt").c_str(), strlen(print_str), print_str, 0, false);
                //sprintf(print_str, "%.4f", load_watthour);
                //mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Load_watthour").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", load_va);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Load_va").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", voltage_bus);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Bus_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", temp_heatsink);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Heatsink_temperature").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", batt_capacity);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_capacity").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.2f", voltage_batt);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", batt_charge_current);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_charge_current").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", batt_discharge_current);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_discharge_current").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%c", device_status[3]);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Load_status_on").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%c", device_status[6]);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_SCC_charge_on").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%c", device_status[7]);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_AC_charge_on").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", batt_recharge_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_recharge_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", batt_under_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_under_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", batt_bulk_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_bulk_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", batt_float_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_float_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", max_grid_charge_current);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Max_grid_charge_current").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", max_charge_current);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Max_charge_current").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", out_source_priority);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Out_source_priority").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%d", charger_source_priority);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Charger_source_priority").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%.1f", batt_redischarge_voltage);
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Battery_redischarge_voltage").c_str(), strlen(print_str), print_str, 0, false);
                sprintf(print_str, "%s", warnings->c_str());
                mosquitto_publish(mosq, NULL, (mqtt_topic + "/sensor/" + mqtt_devicename + "_Warnings").c_str(), strlen(print_str), print_str, 0, false);


                // Delete reply string so we can update with new data when polled again...
                delete reply1;
                delete reply2;
                delete warnings;
            }
        } else if (ups_leave) {
            ups->terminateThread();
            // Do once and exit instead of loop endlessly
            lprintf("INVERTER: All queries complete, exiting loop.");
            exit(0);
        }

        sleep(1);
    }

    // Pulizia e uscita
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    if (ups) {
        ups->terminateThread();
        delete ups;
    }
    return 0;
}
