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


bool debugFlag = false;
bool runOnce = false;

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
float ampfactor;
float wattfactor;
int qpiri, qpiws, qmod, qpigs;

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
    if( access( "./inverter.conf", F_OK ) != -1 ) { // file exists
        settings = "./inverter.conf";
    } else { // file doesn't exist
        settings = "/etc/inverter/inverter.conf";
    }
    getSettingsFile(settings);
    int fd = open(settings, O_RDWR);
    while (flock(fd, LOCK_EX)) sleep(1);

    bool ups_status_changed(false);
    ups = new cInverter(devicename,qpiri,qpiws,qmod,qpigs);

    // Logic to send 'raw commands' to the inverter..
    if (!rawcmd.empty()) {
        int replylen;
        if (!strcmp(rawcmd.c_str(), "QPI"))
            replylen = 8;
        else if (!strncmp(rawcmd.c_str(), "QPGS", 4))
            replylen = 133;
        else if (!strcmp(rawcmd.c_str(), "QID"))
            replylen = 18;
        else if (!strcmp(rawcmd.c_str(), "QVFW"))
            replylen = 18;
        else if (!strcmp(rawcmd.c_str(), "QVFW2"))
            replylen = 19;
        else if (!strcmp(rawcmd.c_str(), "QVFW3"))
            replylen = 19;
        else if (!strcmp(rawcmd.c_str(), "QVFW4"))
            replylen = 19;
        else if (!strcmp(rawcmd.c_str(), "QFLAG"))
            replylen = 15;
        else if (!strcmp(rawcmd.c_str(), "QBOOT"))
            replylen = 5;
        else if (!strcmp(rawcmd.c_str(), "QOPM"))
            replylen = 6;
	else if (!strcmp(rawcmd.c_str(), "QMOD"))
            replylen = qmod;
	else if (!strcmp(rawcmd.c_str(), "QPIRI"))
            replylen = qpiri;
	else if (!strcmp(rawcmd.c_str(), "QPIGS"))
            replylen = qpigs;
	else if (!strcmp(rawcmd.c_str(), "QPIGS2"))
            replylen = 71;
	else if (!strcmp(rawcmd.c_str(), "QDI"))
            replylen = 83;
	else if (!strcmp(rawcmd.c_str(), "QCST"))
            replylen = 6;
	else if (!strcmp(rawcmd.c_str(), "QCVT"))
            replylen = 7;
	else if (!strcmp(rawcmd.c_str(), "QBEQI"))
            replylen = 37;
	else if (!strcmp(rawcmd.c_str(), "QPIWS"))
            replylen = qpiws;
        else replylen = 7;
        ups->ExecuteCmd(rawcmd, replylen);
        // We're piggybacking off the qpri status response...
        printf("Reply:  %s\n", ups->GetQpiriStatus()->c_str());
        exit(0);
    }


    if (runOnce)
        ups->poll();
    else
        ups->runMultiThread();

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
            string *reply1   = ups->GetQpigsStatus();
            string *reply2   = ups->GetQpiriStatus();
            char *warnings = ups->GetWarnings();

            if (reply1 && reply2 && warnings) {

                // Parse and display values, QPIGS, * means contained in output, ^ is not included in output
                sscanf(reply1->c_str(), "%f %f %f %f %d %d %d %d %f %d %d %d %f %f %f %d %s",
                       &voltage_grid,          // * Grid voltage
                       &freq_grid,             // * Grid frequency
                       &voltage_out,           // * AC output voltage
                       &freq_out,              // * AC output frequency
                       &load_va,               // * AC output apparent power (VA)
                       &load_watt,             // * AC output active power (Watt)
                       &load_percent,          // * Output load percent - Maximum of W% or VA%., VA% is a percent of apparent power., W% is a percent of active power.
                       &voltage_bus,           // * BUS voltage
                       &voltage_batt,          // * Battery voltage
                       &batt_charge_current,   // * Battery charging current
                       &batt_capacity,         // * Battery capacity
                       &temp_heatsink,         // * Inverter heat sink temperature
                       &pv_input_current,      // * PV Input current for battery.
                       &pv_input_voltage,      // * PV Input voltage 1
                       &scc_voltage,           // * Battery voltage from SCC (V)
                       &batt_discharge_current,// * Battery discharge current
                       &device_status);        //
                char parallel_max_num; //QPIRI
                sscanf(reply2->c_str(), "%f %f %f %f %f %d %d %f %f %f %f %f %d %d %d %d %d %d %c %d %d %d %f",
                       &grid_voltage_rating,      // ^ Grid rating voltage
                       &grid_current_rating,      // ^ Grid rating current per protocol, frequency in practice
                       &out_voltage_rating,       // ^ AC output rating voltage
                       &out_freq_rating,          // ^ AC output rating frequency
                       &out_current_rating,       // ^ AC output rating current
                       &out_va_rating,            // ^ AC output rating apparent power
                       &out_watt_rating,          // ^ AC output rating active power
                       &batt_rating,              // ^ Battery rating voltage
                       &batt_recharge_voltage,    // * Battery re-charge voltage
                       &batt_under_voltage,       // * Battery under voltage
                       &batt_bulk_voltage,        // * Battery bulk voltage
                       &batt_float_voltage,       // * Battery float voltage
                       &batt_type,                // ^ Battery type - 0 AGM, 1 Flooded, 2 User
                       &max_grid_charge_current,  // * Current max AC charging current
                       &max_charge_current,       // * Current max charging current
                       &in_voltage_range,         // ^ Input voltage range, 0 Appliance 1 UPS
                       &out_source_priority,      // * Output source priority, 0 Utility first, 1 solar first, 2 SUB first
                       &charger_source_priority,  // * Charger source priority 0 Utility first, 1 solar first, 2 Solar + utility, 3 only solar charging permitted
                       &parallel_max_num,         // ^ Parallel max number 0-9
                       &machine_type,             // ^ Machine type 00 Grid tie, 01 Off grid, 10 hybrid
                       &topology,                 // ^ Topology  0 transformerless 1 transformer
                       &out_mode,                 // ^ Output mode 00: single machine output, 01: parallel output, 02: Phase 1 of 3 Phase output, 03: Phase 2 of 3 Phase output, 04: Phase 3 of 3 Phase output
                       &batt_redischarge_voltage);// * Battery re-discharge voltage
                                                  // ^ PV OK condition for parallel
                                                  // ^ PV power balance

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

                // Print as JSON (output is expected to be parsed by another tool...)
                printf("{\n");

                printf("  \"Inverter_mode\":%d,\n", mode);
                printf("  \"AC_grid_voltage\":%.1f,\n", voltage_grid);    // QPIGS
                printf("  \"AC_grid_frequency\":%.1f,\n", freq_grid);     // QPIGS
                printf("  \"AC_out_voltage\":%.1f,\n", voltage_out);      // QPIGS
                printf("  \"AC_out_frequency\":%.1f,\n", freq_out);       // QPIGS
                printf("  \"PV_in_voltage\":%.1f,\n", pv_input_voltage);  // QPIGS
                printf("  \"PV_in_current\":%.1f,\n", pv_input_current);  // QPIGS
                printf("  \"PV_in_watts\":%.1f,\n", pv_input_watts);      // = (scc_voltage * pv_input_current) * wattfactor;
                printf("  \"SCC_voltage\":%.4f,\n", scc_voltage);         // QPIGS
                printf("  \"Load_pct\":%d,\n", load_percent);             // QPIGS
                printf("  \"Load_watt\":%d,\n", load_watt);               // QPIGS
                printf("  \"Load_va\":%d,\n", load_va);                   // QPIGS
                printf("  \"Bus_voltage\":%d,\n", voltage_bus);           // QPIGS
                printf("  \"Heatsink_temperature\":%d,\n", temp_heatsink);// QPIGS
                printf("  \"Battery_capacity\":%d,\n", batt_capacity);    // QPIGS
                printf("  \"Battery_voltage\":%.2f,\n", voltage_batt);    // QPIGS
                printf("  \"Battery_charge_current\":%d,\n", batt_charge_current); // QPIGS 
                printf("  \"Battery_discharge_current\":%d,\n", batt_discharge_current); // QPIGS
                printf("  \"Load_status_on\":%c,\n", device_status[3]);   //
                printf("  \"SCC_charge_on\":%c,\n", device_status[6]);    //
                printf("  \"AC_charge_on\":%c,\n", device_status[7]);     //
                printf("  \"Battery_recharge_voltage\":%.1f,\n", batt_recharge_voltage); // QPIRI
                printf("  \"Battery_under_voltage\":%.1f,\n", batt_under_voltage); // QPIRI
                printf("  \"Battery_bulk_voltage\":%.1f,\n", batt_bulk_voltage);  // QPIRI
                printf("  \"Battery_float_voltage\":%.1f,\n", batt_float_voltage); // QPIRI 
                printf("  \"Max_grid_charge_current\":%d,\n", max_grid_charge_current); // QPIRI
                printf("  \"Max_charge_current\":%d,\n", max_charge_current);  // QPIRI
                printf("  \"Out_source_priority\":%d,\n", out_source_priority); // QPIRI
                printf("  \"Charger_source_priority\":%d,\n", charger_source_priority); // QPIRI
                printf("  \"Battery_redischarge_voltage\":%.1f,\n", batt_redischarge_voltage);  // QPIRI
                printf("  \"Warnings\":\"%s\"\n", warnings);     //
                printf("}\n");

                // Delete reply string so we can update with new data when polled again...
                delete reply1;
                delete reply2;

                if(runOnce) {
                    // there is no thread -- ups->terminateThread();
                    // Do once and exit instead of loop endlessly
                    lprintf("INVERTER: All queries complete, exiting loop.");
                    exit(0);
                }
            }
            free(warnings);
        }

        sleep(1);
    }

    if (ups) {
        ups->terminateThread();
        delete ups;
    }
    return 0;
}
