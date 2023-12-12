#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <vector>
#include "json.hpp" 
#include "mqtt/client.h" 
#include <iomanip>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <boost/asio.hpp>

#define QOS 1
#define BROKER_ADDRESS "tcp://localhost:1883"
#define GRAPHITE_HOST "graphite"
#define GRAPHITE_PORT 2003

std::mutex m;
using namespace boost::asio;
std::time_t convert_to_epoch(const std::string& timestamp_str) {
    // Crie um objeto de tempo usando o timestamp fornecido
    std::tm tm = {};
    std::istringstream ss(timestamp_str);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ"); // Formato do timestamp
    // Converta o objeto de tempo para Unix epoch
    std::time_t epoch_time = std::mktime(&tm);
    return epoch_time;
}
void post_metric(const std::string& machine_id, const std::string& sensor_id, const std::string& timestamp_str, double value) {
    //std::string path = machine_id + '.' + sensor_id;
    //std::string metric = path + " " + std::to_string(value) + " " + timestamp_str;
    std::string metric = machine_id + '.' + sensor_id;
    //std::cout << "valor"<< value <<std::endl;
    std::time_t epoch = convert_to_epoch(timestamp_str);
    std::string message = metric + " " + std::to_string(value) + " " + std::to_string(epoch) + "\n";
    io_context io;
    // std::cout << "epoch"<< std::to_string(epoch) <<std::endl;
    // std::cout << std::to_string(std::time(0)) <<std::endl;
    // std::cout << timestamp_str <<std::endl;
    // Crie um endpoint para o servidor de destino
    ip::tcp::endpoint endpoint(ip::address::from_string("127.0.0.1"), 2003);
    // Crie um socket
    ip::tcp::socket socket(io);

    try {
        // Conecte-se ao servidor
        socket.connect(endpoint);
        // Enviar mensagem
        
        boost::system::error_code error;

        write(socket, buffer(message), error);
        if (error) {
            std::cerr << "Erro ao enviar mensagem: " << error.message() << std::endl;
        } else {
        //    std::cout << "Mensagem enviada com sucesso!" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Erro ao conectar: " << e.what() << std::endl;
    }
}

std::vector<std::string> split(const std::string &str, char delim) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string clientId = "clientId";
mqtt::async_client client(BROKER_ADDRESS, clientId);

//Guarda o valor do timestamp da ultima mensagem recebida de cada sensor
//encadeado como: {id_do_sensor: ultimo_timestamp}
std::map<std::string, std::string> actual_timestamps={{"inicial","true"}};

struct SensorReadings {
    std::vector<double> readings;
};

std::map<std::string, SensorReadings> sensorData;

void monitor_sensor_inactivity(std::string sensorId, int data_interval, std::string machineId) {
    bool data_received = true;
    int count = 0; // Contador de tempo sem dados
    std::string key = machineId+sensorId; 
    auto last_timestamp = actual_timestamps[key]; // Obtém o timestamp inicial

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(data_interval)); // Aguarda 1 intervalo

        m.lock(); // Bloqueia o mutex para acessar actual_timestamps
        auto current_timestamp = actual_timestamps[key];
        m.unlock(); // Libera o mutex
        
        if (current_timestamp == last_timestamp) {
            data_received = false;
            count++;
            //std::string seconds=convert_to_epoch(current_timestamp) + count*(data_interval/1000);
            if (count == 10) {
                std::cout << "Não houve recebimento de dados da maquina"<<machineId << "do" << sensorId << " por 10 intervalos!" << std::endl;
                post_metric(machineId, "alarm.inactivity."+sensorId, current_timestamp, 1);
                count = 0;
            }
        } else {
            data_received = true;
            std::cout << "Dados do maquina" << machineId <<"e sensor " << sensorId << " estão sendo recebidos." << std::endl;
            post_metric(machineId, "alarm.inactivity."+sensorId, current_timestamp, 0);
            count = 0;
        }
        // Atualiza o último timestamp
        last_timestamp = current_timestamp;
        
    }
}

double calculate_average(const std::string& key) {
    double sum = 0.0;
    int count = 0;

    // Verifica se há leituras armazenadas para o sensor
    if (sensorData.find(key) != sensorData.end()) {
        auto& readings = sensorData[key].readings;

        // Calcula a média das últimas 10 leituras, se houverem
        int start_idx = readings.size() > 10 ? readings.size() - 10 : 0;
        for (int i = start_idx; i < readings.size(); ++i) {
            sum += readings[i];
            count++;
        }
    }

    return count > 0 ? sum / count : 0.0;
}
void add_reading(const std::string& key, double value) {
    // Verifica se há leituras armazenadas para o sensor
    if (sensorData.find(key) != sensorData.end()) {
        auto& readings = sensorData[key].readings;

        // Mantém apenas as últimas 10 leituras
        if (readings.size() >= 10) {
            readings.erase(readings.begin());
        }

        // Adiciona a nova leitura
        readings.push_back(value);
    } else {
        // Se não houver leituras armazenadas, cria uma entrada para o sensor
        SensorReadings sr;
        sr.readings.push_back(value);
        sensorData.insert({ key, sr });
    }
}


int main(int argc, char* argv[]) {
    
    // Create an MQTT callback.
    class callback : public virtual mqtt::callback {
    public:

        void message_arrived(mqtt::const_message_ptr msg) override {
            auto j = nlohmann::json::parse(msg->get_payload());

            //std::cout << "topico: " << msg->get_topic() << "    payload: " << msg->get_payload() << std::endl;

            

            if (msg->get_topic() == "/sensor_monitors") {
                
                if (actual_timestamps["inicial"]!="false"){
                    std::string new_machine_id = j["machine_id"];
            
                    std::string new_sensor1_id = j["sensors"][0]["sensor_id"];
                    std::string new_sensor1_data_type = j["sensors"][0]["data_type"];
                    int new_sensor1_interval = j["sensors"][0]["data_interval"];

                    std::string new_sensor2_id = j["sensors"][1]["sensor_id"];
                    std::string new_sensor2_data_type = j["sensors"][1]["data_type"];
                    int new_sensor2_interval = j["sensors"][1]["data_interval"];

                    std::string topic1 = "/sensor_monitors/" + new_machine_id + "/" + new_sensor1_id;
                    std::string topic2 = "/sensor_monitors/" + new_machine_id + "/" + new_sensor2_id;

                    client.subscribe(topic1, QOS);
                    client.subscribe(topic2, QOS);
                    actual_timestamps.insert_or_assign("inicial","false");

                    std::string chave1 = new_machine_id + new_sensor1_id;
                    std::string chave2 = new_machine_id + new_sensor2_id;
                    actual_timestamps.insert_or_assign(chave1, "0T00:00:00");
                    actual_timestamps.insert_or_assign(chave2, "0T00:00:00");
                    std::thread m_i_1(monitor_sensor_inactivity, new_sensor1_id , new_sensor1_interval, new_machine_id);
                    m_i_1.detach();
                    std::thread m_i_2(monitor_sensor_inactivity, new_sensor2_id, new_sensor2_interval, new_machine_id);
                    m_i_2.detach();
                }
            }

            else {
            
            std::string topic = msg->get_topic();
            auto topic_parts = split(topic, '/');
            std::string machine_id = topic_parts[2];
            std::string sensor_id = topic_parts[3];

            std::string timestamp = j["timestamp"];
            double value = j["value"];
    
            std::string key = machine_id + sensor_id;
            actual_timestamps.insert_or_assign(key, timestamp);
           

            post_metric(machine_id, sensor_id, timestamp, value);
            // Adicionar a nova leitura aos dados do sensor
            add_reading(key, value);
            double avg = calculate_average(key);
            if (sensor_id=="sensor1") {
                if(avg > 2.7){
                    std::cout << "A média das leituras para o " << sensor_id << "da maquina "<< machine_id << " é maior que 2.7!" << std::endl;
                    post_metric(machine_id, "alarm.high_frequency", timestamp, 1);
                }else{
                    post_metric(machine_id, "alarm.high_frequency", timestamp, 0);
                }
            }
            }
        }
    };

    

    callback cb;
    client.set_callback(cb);

    // Connect to the MQTT broker.
    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(20);
    connOpts.set_clean_session(true);

    try {
        client.connect(connOpts)->wait();
        client.subscribe("/sensor_monitors", QOS);
    } catch (mqtt::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    while (true) {
        std::cout << actual_timestamps["sensor1"] << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return EXIT_SUCCESS;
}
