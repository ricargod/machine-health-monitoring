#include <iostream>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>
#include <mutex>
#include <unistd.h>
#include "json.hpp" // json handling
#include "mqtt/client.h" // paho mqtt
#include <iomanip>


#define QOS 1
#define BROKER_ADDRESS "tcp://localhost:1883"

std::string clientId = "sensor-monitor";
mqtt::client client(BROKER_ADDRESS, clientId);

std::mutex m;

double get_cpu_frequency() {
    std::ifstream cpuinfoFile("/proc/cpuinfo");
    std::string line;
    double frequency = 0;

    if (cpuinfoFile.is_open()) {
        while (std::getline(cpuinfoFile, line)) {
            if (line.find("cpu MHz") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    std::string freqStr = line.substr(pos + 1);
                    frequency = std::stod(freqStr);
                    break; // Assuming all cores have the same frequency
                }
            }
        }
        cpuinfoFile.close();
    }
    return(frequency/1000);
}

double get_cpu_usage() {
    std::ifstream statFile("/proc/stat");
    std::string line;

    double cpuUsage = 0;

    if (statFile.is_open()) {
        std::getline(statFile, line);
        statFile.close();

        if (line.substr(0, 3) == "cpu") {
            std::istringstream iss(line);
            std::string cpuLabel;
            iss >> cpuLabel;

            if (cpuLabel == "cpu") {
                // Extrair informações sobre o tempo de CPU
                unsigned long user, nice, system, idle;
                iss >> user >> nice >> system >> idle;

                unsigned long totalTime = user + nice + system + idle;
                cpuUsage = 100.0 * (totalTime - idle) / totalTime;
            }
        }
    }
    return cpuUsage;
}

void read_and_publish_sensor(std::string machineId, std::string sensorId, int data_interval) {

    while(1) {

    // Get the current time in ISO 8601 format.
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_c);
    std::stringstream ss;
    ss << std::put_time(now_tm, "%FT%TZ");
    std::string timestamp = ss.str();

    double value = 0;

    if(sensorId == "sensor1") {
        value = get_cpu_frequency();
    }
    else if(sensorId == "sensor2") {
        value = get_cpu_usage();
    }
    
    

    // Construct the JSON message.
    nlohmann::json j;
    j["timestamp"] = timestamp;
    j["value"] = value;

    // Publish the JSON message to the appropriate topic.
    std::string topic = "/sensor_monitors/" + machineId + "/" + sensorId;
    mqtt::message msg(topic, j.dump(), QOS, false);
    std::clog << "message published - topic: " << topic << " - message: " << j.dump() << std::endl;

    m.lock();
    client.publish(msg);
    m.unlock();
   
    std::this_thread::sleep_for(std::chrono::milliseconds(data_interval));
    } 
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Uso: " << argv[0] << " <data_interval1> <data_interval2>" << std::endl;
        return EXIT_FAILURE;
    }

    int data_interval1 = std::atoi(argv[1]);
    int data_interval2 = std::atoi(argv[2]);
    // Connect to the MQTT broker.
    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(20);
    connOpts.set_clean_session(true);

    try {
        client.connect(connOpts);
    } catch (mqtt::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::clog << "connected to the broker" << std::endl;

    // Get the unique machine identifier, in this case, the hostname.
    char hostname[1024];
    gethostname(hostname, 1024);
    std::string machineId(hostname);
    std::string sensor_id1 = "sensor1";
    std::string sensor_id2 = "sensor2";

    nlohmann::json j_sensor1;
    j_sensor1["sensor_id"] = sensor_id1;
    j_sensor1["data_type"] = "cpu_frequency";
    j_sensor1["data_interval"] = data_interval1;

    nlohmann::json j_sensor2;
    j_sensor2["sensor_id"] = sensor_id2;
    j_sensor2["data_type"] = "cpu_usage";
    j_sensor2["data_interval"] = data_interval2;

    nlohmann::json j_inicial;
    j_inicial["machine_id"] = machineId;
    j_inicial["sensors"] = {j_sensor1, j_sensor2};

    std::string topic_inicial = "/sensor_monitors";
    mqtt::message msg_inicial(topic_inicial, j_inicial.dump(), QOS, false);
    //std::clog << "message published - topic: " << topic_inicial << " - message: " << j_inicial.dump() << std::endl;
    client.publish(msg_inicial);

    
    std::thread t_sensor1(read_and_publish_sensor, machineId, sensor_id1, data_interval1);
    std::thread t_sensor2(read_and_publish_sensor, machineId, sensor_id2, data_interval2);

    t_sensor1.join();
    t_sensor2.join();
    
    return EXIT_SUCCESS;
}
