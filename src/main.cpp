#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Adafruit_NeoPixel.h>
#include <DHT20.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_task_wdt.h>

// Wi-Fi and MQTT settings
const char* ssid = "Kurogane";
const char* password = "29122001" ;
const char* mqtt_server = "karis.cloud";
const int mqtt_port = 1883;
const char* data_topic = "test/topic";
const char* time_topic = "time/topic";
const char* task_status = "task/status";

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Components initialization
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);

DHT20 dht20;  // DHT20 sensor
LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD 16x2 I2C at address 0x27
Adafruit_NeoPixel pixels3(4, 5, NEO_GRB + NEO_KHZ800);  // 4 LEDs on GPIO 5

// Task states
enum TaskState { READY, RUNNING, WAITING, COMPLETED };

// Task structure for the doubly linked list
struct Task {
    String name;
    time_t arrivalTime;
    int burstTime;
    int remainingTime;
    String formula;
    int priority;
    TaskState state;
    String recurrence;
    bool enabled;

    Task* next;
    Task* prev;

    Task(String n, time_t at, int bt, String f,  int p, String r, bool e)
        : name(n), arrivalTime(at), burstTime(bt), remainingTime(0), formula(f), priority(p),
          recurrence(r), enabled(e), state(READY), next(nullptr), prev(nullptr) {}
};

// Convert a date-time structure to a time_t value
time_t convertToTimeT(int year, int month, int day, int hour, int minute, int second) {
    struct tm tmStruct = {0};
    tmStruct.tm_year = year - 1900;  // Year since 1900
    tmStruct.tm_mon = month - 1;     // Month: 0-11
    tmStruct.tm_mday = day;
    tmStruct.tm_hour = hour;
    tmStruct.tm_min = minute;
    tmStruct.tm_sec = second;
    return mktime(&tmStruct);  // Convert to time_t
}

int convertToMS(int min, int secs){
    return min * 60000 + secs * 1000;
}

void printFormattedTime(time_t rawTime, char* buffer, size_t bufferSize) {
    struct tm *timeInfo = localtime(&rawTime);
    strftime(buffer, bufferSize, "%d/%m/%Y %H:%M:%S", timeInfo);
}

// Task manager using a doubly linked list
class TaskManager {
public:
    Task* head = nullptr;
    Task* tail = nullptr;

    void addTask(Task* newTask) {
        if (!head) {
            head = tail = newTask;
        } else {
            Task* temp = head;
            while (temp && (temp->formula != newTask->formula || temp->priority >= newTask->priority)) {
                temp = temp->next;
            }
            if (!temp) {
                tail->next = newTask;
                newTask->prev = tail;
                tail = newTask;
            } else if (temp == head) {
                newTask->next = head;
                head->prev = newTask;
                head = newTask;
            } else {
                newTask->next = temp;
                newTask->prev = temp->prev;
                temp->prev->next = newTask;
                temp->prev = newTask;
            }
        }
        char buffer[20];
        printFormattedTime(newTask->arrivalTime, buffer, sizeof(buffer));
        Serial.printf("[INFO] Task %s added with arrival time %s\n", newTask->name, buffer);
    }

    void removeTask(String taskName) {
        Task* task = findTask(taskName);
        if (task) {
            if (task == head) head = task->next;
            if (task == tail) tail = task->prev;
            if (task->prev) task->prev->next = task->next;
            if (task->next) task->next->prev = task->prev;
            Serial.printf("[INFO] Task %s deleted\n", task->name);
            delete task;
            return;
        }
    }

    void toggleTask(String taskName) {
        Task* task = findTask(taskName);
        if (task) {
            task->enabled = !task->enabled;
            Serial.printf("[INFO] Task %s toggled to %s\n", taskName.c_str(), task->enabled ? "enabled" : "disabled");
        }
    }

    void editTask(String taskName, time_t arrivalTime, int burstTime, int priority, String recurrence) {
        Task* task = findTask(taskName);
        if (task) {
            task->arrivalTime = arrivalTime;
            task->burstTime = burstTime;
            task->priority = priority;
            task->recurrence = recurrence;
            Serial.printf("[INFO] Task %s edited\n", taskName.c_str());
        }
    }

    Task* findTask(String taskName) {
        Task* temp = head;
        while (temp) {
            if (temp->name == taskName) return temp;
            temp = temp->next;
        }
        return nullptr;
    }

    Task* getReadyTask(unsigned long currentTime) {
        Task* temp = head;
        while (temp) {
            if (temp->state == READY && temp->arrivalTime <= currentTime) {
                return temp;
            }
            temp = temp->next;
        }
        return nullptr;
    }

    void updateRecurrence(Task* task) {
        if (task->recurrence == "once") {
            task->state = COMPLETED;
            task->enabled = false;  // Disable the task after one execution
            String message = "{\"task\": \"" + task->name + "\", \"status\": \"completed\"}";
            mqttClient.beginMessage(task_status);
            mqttClient.print(message);
            mqttClient.endMessage();

            Serial.printf("[INFO] Task %s marked as completed and notification sent\n", task->name);
        } else if (task->recurrence == "daily") {
            task->state = READY;
            task->arrivalTime += 86400;  // Add 24 hours in seconds
            char buffer[20];
            printFormattedTime(task->arrivalTime, buffer, sizeof(buffer));
            Serial.printf("[INFO] Task %s completed and execute again %s", task->name, buffer);
        } else if (task->recurrence == "weekly") {
            task->state = READY;
            task->arrivalTime += 604800;  // Add 7 days in seconds
            char buffer[20];
            printFormattedTime(task->arrivalTime, buffer, sizeof(buffer));
            Serial.printf("[INFO] Task %s completed and execute again %s", task->name, buffer);
        }
    }

    bool shouldPreempt(Task* runningTask, Task* newTask) {
        // No task is currently running, so the new task can start
        if (!runningTask) {
            return true;
        }

        // Only preempt if the new task has higher priority and matches the formula group
        if (runningTask->formula == newTask->formula) {
            return newTask->priority > runningTask->priority;
        }

        // For different formulas, treat as non-preemptive
        return false;
    }

    bool areAllTasksCompleted(String formula) {
        Task* temp = head;
        while (temp) {
            if (temp->formula == formula && temp->state != COMPLETED) return false;
            temp = temp->next;
        }
        return true;
    }
};

// Preempted task list using a double linked list
class PreemptedTaskList {
private:
    Task* head;
    Task* tail;

public:
    // Constructor to initialize an empty list
    PreemptedTaskList() : head(nullptr), tail(nullptr) {}

    // Method to add a task to the list based on priority
    void addTask(Task* task) {
        if (!head) {
            head = tail = task;
        } else {
            Task* temp = head;
            // Find the correct position based on priority (higher priority goes to the front)
            while (temp && temp->priority >= task->priority) {
                temp = temp->next;
            }
            if (!temp) {
                // Insert at the end
                tail->next = task;
                task->prev = tail;
                tail = task;
            } else if (temp == head) {
                // Insert at the beginning
                task->next = head;
                head->prev = task;
                head = task;
            } else {
                // Insert in the middle
                task->next = temp;
                task->prev = temp->prev;
                temp->prev->next = task;
                temp->prev = task;
            }
        }
    }

    // Method to retrieve and remove the highest-priority task (head)
    Task* popHighestPriorityTask() {
        if (!head) return nullptr;  // List is empty

        Task* highestPriorityTask = head;  // Head has the highest priority
        head = head->next;  // Move head to the next task

        // Update the new head and adjust pointers
        if (head) 
            head->prev = nullptr;
        else
            tail = nullptr;  // If head is null, the list is empty
        
        // Disconnect the popped task from the list
        highestPriorityTask->next = highestPriorityTask->prev = nullptr;

        return highestPriorityTask;  // Return the task for further processing
    }

    // Method to check if the list is empty
    bool isEmpty() const {
        return head == nullptr;
    }

    // Clear all tasks in the list
    void clear() {
        while (head) {
            Task* temp = head;
            head = head->next;
            delete temp;
        }
        tail = nullptr;
    }
};

TaskManager scheduler;
PreemptedTaskList preemptedTasks;
SemaphoreHandle_t taskListMutex;

void taskScheduler(void* pvParameters);
void handleMQTTMessages(String topic, String payload);
void setupWebServer(void* pvParameters);
void wifiTask(void *pvParameters);
void backgroundTask(void* pvParameters);
void taskMQTTClient(void* pvParameters);
void taskMQTTCommunication(void* pvParameters);
void executeTask(Task*& task);

void setup() {
    Serial.begin(115200);

    esp_task_wdt_init(60, true);  // Disable WDT or set to a high timeout
    esp_task_wdt_delete(NULL);    // Remove WDT for the main task if necessary

    // Create tasks for Wi-Fi, server
    xTaskCreate(wifiTask, "WiFiTask", 8192, NULL, 3, NULL);
    xTaskCreate(setupWebServer, "ServerTask", 8192, NULL, 3, NULL);

    Wire.begin(11, 12);
    dht20.begin();
    lcd.begin();
    lcd.backlight();
    pixels3.begin();
    timeClient.begin();

    //lcdMutex = xSemaphoreCreateMutex();
    taskListMutex = xSemaphoreCreateMutex();

    xTaskCreate(backgroundTask, "Background Task", 4096, NULL, 1, NULL);
    xTaskCreate(taskMQTTClient, "MQTTClient", 8192, NULL, 1, NULL);
    xTaskCreate(taskScheduler, "Scheduler", 8192 * 4, NULL, 3, NULL);
    xTaskCreate(taskMQTTCommunication, "Task MQTT Communication", 8192, NULL, 2, NULL);

    /*time_t arrivalTime1 = convertToTimeT(2024, 11, 7, 10, 44, 0);
    Task* newTask1 = new Task("Task 1", arrivalTime1, 10000, "formula 1", 1, "once", true);
    scheduler.addTask(newTask1);

    time_t arrivalTime2 = convertToTimeT(2024, 11, 7, 10, 44, 5);
    Task* newTask2 = new Task("Task 2", arrivalTime2, 10000, "formula 1", 1, "once", true);
    scheduler.addTask(newTask2);

    time_t arrivalTime3 = convertToTimeT(2024, 11, 7, 10, 44, 10);
    Task* newTask3 = new Task("Task 3", arrivalTime3, 10000, "formula 2", 1, "once", true);
    scheduler.addTask(newTask3);*/
    
}

// Task to handle Wi-Fi connection
void wifiTask(void *pvParameters) {
    Serial.printf("Connecting to %s\n", ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);  // Blocking delay for simplicity
        Serial.print(".");
    }
    Serial.print("WiFi connected. IP address: ");
    Serial.println(WiFi.localIP());

    vTaskDelete(NULL);  // Delete the task when done
}

// Web page processor function
String processor(const String &var) {
    Serial.println(var);
    if (var == "TEMPERATURE") {
        return String(dht20.getTemperature());
    }
    if (var == "HUMIDITY") {
        return String(dht20.getHumidity());
    }
    if (var == "MOISTURE") {
        return String(analogRead(0));
    }
    if (var == "LIGHT") {
        return String(analogRead(1));
    }
    return String();
}

// Task to set up the web server
void setupWebServer(void *pvParameters) {
    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.printf("An Error occurred while mounting SPIFFS\n");
        vTaskDelete(NULL);  // Delete the task if SPIFFS initialization fails
    }

    // Define the route for the root web page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", String(), false, processor);
    });

    // Define the route to load the CSS file
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/style.css", "text/css");
    });

    server.on("/sensor", HTTP_GET, [](AsyncWebServerRequest *request) {
        float temperature = dht20.getTemperature();
        float humidity = dht20.getHumidity();
        int moisture = analogRead(1);
        int light = analogRead(2);
        
        String json = "{\"temperature\": " + String(temperature, 2) + 
                      ", \"humidity\": " + String(humidity, 2) + 
                      ", \"moisture\": " + String(moisture) + 
                      ", \"light\": " + String(light) + "}";
        request->send(200, "application/json", json);
    });

    // Start the server
    server.begin();
    vTaskDelete(NULL);  // Delete the task after setting up the server
}

void taskScheduler(void* pvParameters) {
    Task* runningTask = nullptr;
    String currentFormula = "";  // Track the formula currently executing

    while (true) {
        timeClient.update();
        time_t currentTime = timeClient.getEpochTime();
        char buffer[20];
        printFormattedTime(currentTime, buffer, sizeof(buffer));
        Serial.printf("[INFO] Current time is: %s\n", buffer);

        if (!runningTask && !preemptedTasks.isEmpty()) {
            // Resume from preempted tasks if there is no current running task
            if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
                runningTask = preemptedTasks.popHighestPriorityTask();
                if (runningTask) {
                    currentFormula = runningTask->formula;  // Set the current formula context
                    Serial.printf("[INFO] Resuming preempted task: %s with remaining time: %d ms. Formula: %s\n",
                                  runningTask->name.c_str(), runningTask->remainingTime, currentFormula.c_str());
                }
                xSemaphoreGive(taskListMutex);
            }
        }

        if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
            // Get the next ready task based on time and priority
            Task* readyTask = scheduler.getReadyTask(currentTime);

            if (readyTask) {
                // Check if the ready task's formula matches the current formula
                if (currentFormula == "" || readyTask->formula == currentFormula) {
                    // Same formula or first task setting the context; allows preemption within the formula
                    if (!runningTask || scheduler.shouldPreempt(runningTask, readyTask)) {
                        if (runningTask) {
                            Serial.printf("[WARNING] Preempting task %s for task %s.\n",
                                          runningTask->name.c_str(), readyTask->name.c_str());
                            preemptedTasks.addTask(runningTask);
                        }
                        runningTask = readyTask;
                        currentFormula = runningTask->formula;
                        runningTask->remainingTime = runningTask->burstTime;  // Initialize remaining time
                        runningTask->state = RUNNING;
                        Serial.printf("[INFO] Running task: %s with remaining time: %d ms. Formula: %s\n",
                                      runningTask->name.c_str(), runningTask->remainingTime, currentFormula.c_str());
                    }
                } else {
                    // Different formula - check if all tasks with current formula are done
                    if (scheduler.areAllTasksCompleted(currentFormula)) {
                        // Switch to the new formula
                        Serial.printf("[INFO] All tasks for the current formula (%s) are completed, resetting formula context\n", currentFormula.c_str());
                        currentFormula = readyTask->formula;
                        runningTask = readyTask;
                        runningTask->remainingTime = runningTask->burstTime;
                        runningTask->state = RUNNING;
                        Serial.printf("[INFO] Switching to formula %s, running task: %s with remaining time: %d ms\n",
                                      currentFormula.c_str(), runningTask->name.c_str(), runningTask->remainingTime);
                    } else {
                        Serial.printf("[INFO] Waiting for tasks with formula %s to complete before switching to formula %s\n", 
                                      currentFormula.c_str(), readyTask->formula.c_str());
                    }
                }
            }
            xSemaphoreGive(taskListMutex);
        }

        // Execute the running task if available
        if (runningTask) {
            executeTask(runningTask);

            // Check if the task completed and switch to the next preempted task
            if (!runningTask) {  // Task completed
                if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
                    // Check if there are any remaining preempted tasks with the current formula
                    if (!preemptedTasks.isEmpty()) {
                        runningTask = preemptedTasks.popHighestPriorityTask();
                        if (runningTask) {
                            currentFormula = runningTask->formula;  // Set the formula to the preempted task's formula
                            Serial.printf("[INFO] Resuming preempted task: %s with remaining time: %d ms. Formula: %s\n",
                                          runningTask->name.c_str(), runningTask->remainingTime, currentFormula.c_str());
                        }
                    } else if (scheduler.areAllTasksCompleted(currentFormula)) {
                        // If no preempted tasks, reset currentFormula to allow the next formula to run
                        Serial.printf("[INFO] All tasks for formula %s are completed. Ready for new formula.\n", currentFormula.c_str());
                        currentFormula = "";
                    }
                    xSemaphoreGive(taskListMutex);
                }
            }
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);  // Small delay to prevent overloading
    }
}

void executeTask(Task*& task) {
    static bool relayInitialized = false;
    if (!relayInitialized) {
        pinMode(6, OUTPUT);
        relayInitialized = true;
        Serial.println("[DEBUG] Relay initialized.");
    }

    if (task->remainingTime == 0){
        task->remainingTime = task->burstTime;
    }

    esp_task_wdt_reset();
    int loopCounter = 0;
    digitalWrite(6, HIGH);  // Activate relay

    while (task && task->remainingTime > 0) {
        loopCounter++;
        int timeChunk = min(100, task->remainingTime);
        Serial.printf("[INFO] Running task: %s (Remaining time: %d ms), Loop Count: %d\n", 
                      task->name.c_str(), task->remainingTime, loopCounter);

        esp_task_wdt_reset();  // Reset watchdog periodically
        vTaskDelay(timeChunk / portTICK_PERIOD_MS);
        task->remainingTime -= timeChunk;

        // Check if a higher-priority task is ready to preempt this one
        if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
            Task* preemptingTask = scheduler.getReadyTask(timeClient.getEpochTime());
            if (preemptingTask && 
                preemptingTask->formula == task->formula &&
                scheduler.shouldPreempt(task, preemptingTask)) {
                Serial.printf("[WARNING] Task %s preempted by higher-priority task %s\n", 
                              task->name.c_str(), preemptingTask->name.c_str());
                preemptedTasks.addTask(task);  // Save current task state
                task = preemptingTask;  // Switch to preempting task
                xSemaphoreGive(taskListMutex);
                return;  // Exit current task execution
            }
            xSemaphoreGive(taskListMutex);
        }

        if (!task || task->remainingTime <= 0) break;
    }

    if (task && task->remainingTime <= 0) {
        task->remainingTime = 0;
        task->state = COMPLETED;
        Serial.printf("[INFO] Task %s completed.\n", task->name.c_str());

        // Update the task's recurrence if needed
        scheduler.updateRecurrence(task);

        digitalWrite(6, LOW);  // Deactivate relay
        Serial.printf("[DEBUG] Relay OFF - Task %s completed.\n", task->name.c_str());
        task = nullptr;  // Task has finished execution
    }
}

void backgroundTask(void* pvParameters) {
    pinMode(6, OUTPUT);  // Chân điều khiển thiết bị, ví dụ bơm nước
    bool lightState = false;  // Biến theo dõi trạng thái đèn LED
    pixels3.begin(); // Khởi tạo Neopixel

    while (true) {
        dht20.read();  // Đọc cảm biến DHT20
        float temperature = dht20.getTemperature();
        float humidity = dht20.getHumidity();
        int moisture = analogRead(1);  // Đọc cảm biến độ ẩm đất
        int light = analogRead(2);     // Đọc cảm biến ánh sáng

        // Điều khiển thiết bị theo độ ẩm đất
        if (moisture > 500) {
            digitalWrite(6, LOW);
        }
        if (moisture < 50) {
            digitalWrite(6, HIGH);
        }

        // Điều khiển LED Neopixel theo ánh sáng với biến trạng thái
        if (light < 350 && !lightState) {
            for (int i = 0; i < 4; i++) {
                pixels3.setPixelColor(i, pixels3.Color(255, 0, 0)); // Màu đỏ
            }
            pixels3.show();
            lightState = true;  // Cập nhật trạng thái đèn sáng
        }
        if (light > 550 && lightState) {
            for (int i = 0; i < 4; i++) {
                pixels3.setPixelColor(i, pixels3.Color(0, 0, 0)); // Tắt đèn
            }
            pixels3.show();
            lightState = false;  // Cập nhật trạng thái đèn tắt
        }

        vTaskDelay(2000 / portTICK_PERIOD_MS);  // Dừng trong 2 giây
    }
}

void taskMQTTClient(void* pvParameters) {
    int retryCount = 0;
    const int MAX_RETRIES = 5;

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!mqttClient.connected()) {
                Serial.printf("Connecting to MQTT...\n");
                if (mqttClient.connect(mqtt_server, mqtt_port)) {
                    Serial.printf("[INFO] Connected to MQTT server: %s\n", mqtt_server);
                    mqttClient.subscribe(data_topic);
                    mqttClient.subscribe(time_topic);
                    mqttClient.subscribe(task_status);
                    retryCount = 0;
                } else {
                    Serial.printf("MQTT connection failed\n");
                    retryCount++;
                    vTaskDelay(5000 * retryCount / portTICK_PERIOD_MS);  // Exponential backoff
                    if (retryCount >= MAX_RETRIES) retryCount = MAX_RETRIES;
                }
            }
        }
        mqttClient.poll();
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void taskMQTTCommunication(void* pvParameters) {
  while (true) {
    dht20.read();
    // Read sensors
    float temperature = dht20.getTemperature();
    float humidity = dht20.getHumidity();
    int moisture = analogRead(1);
    int light = analogRead(2);

    // Get the MAC address of the ESP32 as device ID
    String device_id = WiFi.macAddress();

    // Construct JSON payload with "device_id" field
    String payload = "{\"device_id\": \"" + device_id + 
                     "\", \"temperature\": " + String(temperature) + 
                     ", \"humidity\": " + String(humidity) + 
                     ", \"moisture\": " + String(moisture) + 
                     ", \"light\": " + String(light) + "}";

    // Publish data to MQTT broker
    mqttClient.beginMessage(data_topic);
    mqttClient.print(payload);
    bool publishSuccess = mqttClient.endMessage();

    if (publishSuccess) {
      Serial.print("[INFO] Successfully published message: ");
      Serial.println(payload);
    } else {
      Serial.println("[ERROR] Failed to publish message.");
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); 
  }
}

void handleMQTTMessages(String topic, String payload) {
    if (topic == time_topic){
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        String action = doc["action"];
        String taskName = doc["taskName"];
        int year = doc["year"];
        int month = doc["month"];
        int day = doc["day"];
        int hour = doc["hour"];
        int minute = doc["minute"];
        int second = doc["second"];
        int burstMin = doc["burstMin"];
        int burstSec = doc["burstSec"];
        String formula = doc["formula"];
        int priority = doc["priority"];
        String recurrence = doc["recurrence"];
        bool enabled = doc["enabled"];

        time_t arrivalTime = convertToTimeT(year, month, day, hour, minute, second);
        int burstTime = convertToMS(burstMin, burstSec);

        if (action == "add") {
            Task* newTask = new Task(taskName, arrivalTime, burstTime, formula, priority, recurrence, enabled);
            if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
                scheduler.addTask(newTask);
                xSemaphoreGive(taskListMutex);
            }
        }
        if (action == "remove") {
            if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
                scheduler.removeTask(taskName);
                xSemaphoreGive(taskListMutex);
            }
        }
        if (action == "toggle") {
            if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
                scheduler.toggleTask(taskName);
                xSemaphoreGive(taskListMutex);
            }
        }
        if (action == "edit") {
            if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
                scheduler.editTask(taskName, arrivalTime, burstTime, priority, recurrence);
                xSemaphoreGive(taskListMutex);
            }
        }
    }
}

void loop() {

}