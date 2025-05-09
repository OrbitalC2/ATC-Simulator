#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <queue>
#include <pthread.h>
#include <unistd.h>
#include <random>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <unistd.h>    
#include <sys/stat.h>   
#include <fcntl.h>  
#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>   
#include "shared_data.h"


using namespace std;
bool disctOutput = true;

//Color for cout
const string RED    = "\033[1;31m";
const string GREEN  = "\033[1;32m";
const string YELLOW = "\033[1;33m";
const string CYAN   = "\033[1;36m";
const string BOLD   = "\033[1m";
const string RESET  = "\033[0m";

//AVN fines:
const int commerical = 500000;
const int cargo = 700000;
const double adminCharge = 1.15;

// Mutexes
pthread_mutex_t coutMutex;
pthread_mutex_t airlineMutex;
pthread_mutex_t queueMutex;
pthread_mutex_t timeMutex;

//simulated boundaries and altitude limits
const double MAX_NORTH_BOUNDARY = 100.0;  
const double MAX_SOUTH_BOUNDARY = -100.0;
const double MAX_EAST_BOUNDARY = 100.0;
const double MAX_WEST_BOUNDARY = -100.0;
const double MAX_ALTITUDE = 40000.0; // ft
const double MIN_ALTITUDE = 1000.0;  

static const int maxPriority = 4;
int globalsimMins = 0;

pthread_cond_t emergencyCondition = PTHREAD_COND_INITIALIZER;

string formatTimestamp(int minutes) {
    int hours = minutes / 60;
    int mins = minutes % 60;
    char buffer[10];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", hours, mins);
    return string(buffer);
}

// Structures
struct Airline {
    string name;
    string type;
    int maxAircraft;
    int maxFlights;
    int inService = 0;
    int flightsUsed = 0;

    bool canLaunch() {
        if (inService < maxAircraft && flightsUsed < maxFlights) {
            ++inService;
            ++flightsUsed;
            return true;
        }
        return false;
    }

    void onComplete() {
        --inService;
    }
};

struct Runway {
    string name;
    pthread_mutex_t mtx;
    bool isOccupied;
    pthread_t currentThread; 
    bool emergencyIncoming;  

    Runway(const string& n) : name(n), isOccupied(false), emergencyIncoming(false) {
        pthread_mutex_init(&mtx, nullptr);
    }
};

struct Flight {
    int    flightNumber;
    string airlineName;
    string type;        // Emergency, Cargo, Commercial
    string category;    // Arrival / Departure
    string direction;   // North/South/East/West
    string phase;
    string status;
    double currentSpeed = 0.0;
    bool avnActivated = false;
    Runway* assignedRunway = nullptr;
    
    double altitude = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    
    int enqueueTime = 0;  
    int estimatedWaitTime = 0; 
    
    pthread_t flightThread;
    bool preempted = false;
    bool yield = false;

    int priorityLevel() const {
        if (type == "Emergency") return 1;
        if (type == "Cargo")     return 2;
        return 3;
    }

    string formattedFlightNumber() const {
        char buf[6];
        snprintf(buf, sizeof(buf), "F%04d", flightNumber);
        return string(buf);
    }
    
    bool checkBoundaryViolation() {
        if (longitude > MAX_EAST_BOUNDARY || longitude < MAX_WEST_BOUNDARY || latitude > MAX_NORTH_BOUNDARY || latitude < MAX_SOUTH_BOUNDARY) {
            return true;
        }
        return false;
    }
    

    bool checkAltitudeViolation() {
        if (phase == "Cruise" && (altitude > MAX_ALTITUDE || altitude < MIN_ALTITUDE)) {
            return true;
        }
        else if (phase == "Climb" && altitude > 30000.0) {
            return true;
        }
        return false;
    }
};

Runway runwayA("RWY-A"), runwayB("RWY-B"), runwayC("RWY-C");

// Queues: FIFO per priority level
vector< queue<Flight*> > arrivalQueues(maxPriority), departureQueues(maxPriority);

// Random helpers
int getRandomInt(int min, int max) {
    return min + rand() % (max - min + 1);
}
double getRandomDouble(double min, double max) {
    return min + (rand() / (double)RAND_MAX) * (max - min);
}


void issueViolationAVN(Flight* f, const string& violationType, double recorded, double limit) {
    AirspaceViolationNotice avn{};
    
    avn.flightNumber = f->flightNumber;
    strncpy(avn.avnID, AirspaceViolationNotice::generateAVNId(avn.flightNumber).c_str(), sizeof(avn.avnID)-1);
    strncpy(avn.airline, f->airlineName.c_str(), sizeof(avn.airline)-1);
    strncpy(avn.aircraftType, f->type.c_str(), sizeof(avn.aircraftType)-1);
    avn.recordedSpeed = recorded;  
    avn.permissibleSpeed = limit;  
    time_t now = time(nullptr);
    strftime(avn.issuanceDateTime, sizeof(avn.issuanceDateTime), "%F %T", localtime(&now));

    time_t due = now + 72*3600;
    strftime(avn.dueDate, sizeof(avn.dueDate), "%F %T", localtime(&due));

    strncpy(avn.paymentStatus, "unpaid", sizeof(avn.paymentStatus)-1);
    avn.paymentStatus[sizeof(avn.paymentStatus)-1] = '\0';

    if(f->type == "Commercial"){
        avn.baseFine = commerical;
    }
    else{
        avn.baseFine = cargo;
    }

    avn.totalAmount = avn.baseFine * adminCharge;
    
    int fd = open(FIFO_SIM2ATC, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        write(fd, &avn, sizeof(avn));
        close(fd);
    }
    
    pthread_mutex_lock(&coutMutex);
    cout << RED << "  [AVN " << violationType << " VIOLATION] for " << f->formattedFlightNumber() 
         << " - Recorded: " << recorded << ", Limit: " << limit << RESET << endl;
    pthread_mutex_unlock(&coutMutex);
}

// Simulation functions
void simulateFaultHandling(Flight* f) {
    if (getRandomInt(1, 10) == 1) {
        if(disctOutput){
            pthread_mutex_lock(&coutMutex);
            cout << RED << "[FAULT] " << f->formattedFlightNumber()
                << " has a ground fault! Requires towing!" << RESET << endl;
            pthread_mutex_unlock(&coutMutex);
        }
        f->status = "Faulted";
    }
    
    if (f->yield) {
        pthread_mutex_lock(&coutMutex);
        cout << YELLOW << "[PREEMPTED] " << f->formattedFlightNumber() 
             << " yielding for emergency!" << RESET << endl;
        pthread_mutex_unlock(&coutMutex);
     
        sleep(2);
        f->yield = false;
    }
}

void checkPhase(Flight* f, const string& phaseName, double minSpeed, double maxSpeed) {
    f->phase = phaseName;
    f->status = phaseName;
    double span = maxSpeed - minSpeed;
    double bias = span * 0.1;
    double low  = minSpeed - bias;
    double high = maxSpeed + bias;
    f->currentSpeed = getRandomDouble(low, high);
    

    if (phaseName == "Cruise") {
        f->altitude = getRandomDouble(30000.0, 42000.0);  // Cruise 
    } 
    else if (phaseName == "Climb") {
        f->altitude = getRandomDouble(10000.0, 30000.0);  // Climbing 
    } 
    else if (phaseName == "Approach") {
        f->altitude = getRandomDouble(1000.0, 10000.0);   // Approach 
    } 
    else {
        f->altitude = getRandomDouble(0.0, 1000.0);       // Ground 
    }

    if (f->direction == "North") {
        f->latitude = getRandomDouble(0.0, MAX_NORTH_BOUNDARY * 1.1);  // Potentially violate boundary
        f->longitude = getRandomDouble(MAX_WEST_BOUNDARY, MAX_EAST_BOUNDARY);
    } 
    else if (f->direction == "South") {
        f->latitude = getRandomDouble(MAX_SOUTH_BOUNDARY * 1.1, 0.0);
        f->longitude = getRandomDouble(MAX_WEST_BOUNDARY, MAX_EAST_BOUNDARY);
    } 
    else if (f->direction == "East") {
        f->longitude = getRandomDouble(0.0, MAX_EAST_BOUNDARY * 1.1);
        f->latitude = getRandomDouble(MAX_SOUTH_BOUNDARY, MAX_NORTH_BOUNDARY);
    } 
    else {
        f->longitude = getRandomDouble(MAX_WEST_BOUNDARY * 1.1, 0.0);
        f->latitude = getRandomDouble(MAX_SOUTH_BOUNDARY, MAX_NORTH_BOUNDARY);
    }

    if(disctOutput){
        pthread_mutex_lock(&coutMutex);
        cout << CYAN << "[" << formatTimestamp(globalsimMins) << "][PHASE] "
            << f->formattedFlightNumber() << " | " << phaseName
            << " | " << f->currentSpeed << " km/h | Alt: " << (int)f->altitude << " ft" << RESET << endl;
        pthread_mutex_unlock(&coutMutex);

        // Check speed violations
        if (f->currentSpeed < minSpeed || f->currentSpeed > maxSpeed) {
            f->avnActivated = true;
    
            AirspaceViolationNotice avn{};

            avn.flightNumber = f->flightNumber;
            strncpy(avn.avnID, AirspaceViolationNotice::generateAVNId(avn.flightNumber).c_str(), sizeof(avn.avnID)-1);
            strncpy(avn.airline, f->airlineName.c_str(), sizeof(avn.airline)-1);
            strncpy(avn.aircraftType, f->type.c_str(), sizeof(avn.aircraftType)-1);
            avn.recordedSpeed = f->currentSpeed;
            avn.permissibleSpeed = AirspaceViolationNotice::phaseMaxSpeed(f->phase);
            time_t now = time(nullptr);
            strftime(avn.issuanceDateTime, sizeof(avn.issuanceDateTime), "%F %T", localtime(&now));

            time_t due = now + 72*3600;
            strftime(avn.dueDate, sizeof(avn.dueDate), "%F %T", localtime(&due));

            strncpy(avn.paymentStatus, "unpaid", sizeof(avn.paymentStatus)-1);
            avn.paymentStatus[sizeof(avn.paymentStatus)-1] = '\0';

            if(f->type == "Commercial"){
                avn.baseFine = commerical;
            }
            else{
                avn.baseFine = cargo;
            }

            avn.totalAmount = avn.baseFine * adminCharge;
            
            int fd = open(FIFO_SIM2ATC, O_WRONLY | O_NONBLOCK);
            if (fd >= 0) {
                write(fd, &avn, sizeof(avn));
                close(fd);
            }
    
            pthread_mutex_lock(&coutMutex);
            cout << RED << "  [AVN SPEED VIOLATION] during " << phaseName << " for " << f->formattedFlightNumber() << RESET << endl;
            pthread_mutex_unlock(&coutMutex);
        }
        
        if (f->checkBoundaryViolation()) {
            f->avnActivated = true;
            double boundary = 0.0;
            if (f->longitude > MAX_EAST_BOUNDARY){
                boundary = MAX_EAST_BOUNDARY;
            }
            else if (f->longitude < MAX_WEST_BOUNDARY){
                boundary = MAX_WEST_BOUNDARY;
            }
            else if (f->latitude > MAX_NORTH_BOUNDARY){
                boundary = MAX_NORTH_BOUNDARY;
            }
            else{
                boundary = MAX_SOUTH_BOUNDARY;
            }
            
            double position = (f->longitude > MAX_EAST_BOUNDARY || f->longitude < MAX_WEST_BOUNDARY) ? f->longitude : f->latitude;
            issueViolationAVN(f, "BOUNDARY", position, boundary);
        }
        
        if (f->checkAltitudeViolation()) {
            f->avnActivated = true;
            double limit = (f->altitude > MAX_ALTITUDE) ? MAX_ALTITUDE : MIN_ALTITUDE;
            issueViolationAVN(f, "ALTITUDE", f->altitude, limit);
        }
    }
    
    if (f->yield) {
        pthread_mutex_lock(&coutMutex);
        cout << YELLOW << "[YIELD] " << f->formattedFlightNumber() 
             << " yielding for emergency during " << phaseName << RESET << endl;
        pthread_mutex_unlock(&coutMutex);
        f->yield = false;
    }
    
    sleep(4);
}

void animateLandingRollout(Flight* f) {
    pthread_mutex_lock(&coutMutex);
    cout << GREEN << "\nLanding Rollout: " << f->formattedFlightNumber()
         << RESET << endl;
    pthread_mutex_unlock(&coutMutex);

    for (int i = 0; i < 5; ++i) {
        f->currentSpeed -= 42;
        usleep(30000);
        pthread_mutex_lock(&coutMutex);
        cout << "  | Speed: " << f->currentSpeed << " km/h" << endl;
        pthread_mutex_unlock(&coutMutex);
        
        if (f->yield && i < 4) {
            pthread_mutex_lock(&coutMutex);
            cout << YELLOW << "[EXPEDITING] " << f->formattedFlightNumber() 
                 << " expediting rollout for emergency!" << RESET << endl;
            pthread_mutex_unlock(&coutMutex);
            break;  // Exit rollout early if emergency needs runway
        }
    }

    if (f->currentSpeed > 30) {
        f->avnActivated = true;
        pthread_mutex_lock(&coutMutex);
        cout << RED << "  [Landing Violation] "
             << f->formattedFlightNumber() << RESET << endl;
        pthread_mutex_unlock(&coutMutex);
    }
    sleep(2);
}

void* simulateArrivalFlight(void* arg) {
    Flight* f = (Flight*)arg;

    if (f->direction == "North" || f->direction == "South")
        f->assignedRunway = &runwayA;
    else
        f->assignedRunway = &runwayC;

    pthread_mutex_lock(&f->assignedRunway->mtx);
    f->assignedRunway->isOccupied = true;
    f->assignedRunway->currentThread = pthread_self();

    pthread_mutex_lock(&coutMutex);
    cout << GREEN << "[" << formatTimestamp(globalsimMins) << "][ARRIVAL][" << f->type << "] "
         << f->formattedFlightNumber() << " on "
         << f->assignedRunway->name << RESET << endl;
    pthread_mutex_unlock(&coutMutex);

    checkPhase(f, "Holding", 400, 600);
    checkPhase(f, "Approach", 240, 290);

    pthread_mutex_lock(&coutMutex);
    cout << BOLD << "\nLanding | " << f->formattedFlightNumber()
         << " | 240 km/h" << RESET << endl;
    pthread_mutex_unlock(&coutMutex);

    f->currentSpeed = 240;
    animateLandingRollout(f);

    checkPhase(f, "Taxi", 15, 30);
    simulateFaultHandling(f);
    checkPhase(f, "AtGate", 0, 5);

    f->assignedRunway->isOccupied = false;
    pthread_mutex_unlock(&f->assignedRunway->mtx);

    pthread_mutex_lock(&coutMutex);
    cout << GREEN << "[COMPLETE] " << f->formattedFlightNumber()
         << " cleared " << f->assignedRunway->name << RESET << endl
         << CYAN << "------------------------------------------------\n"
         << RESET;
    pthread_mutex_unlock(&coutMutex);

    return nullptr;
}

void* simulateDepartureFlight(void* arg) {
    Flight* f = (Flight*)arg;

    if (f->type == "Cargo")
        f->assignedRunway = &runwayC;
    else
        f->assignedRunway = &runwayB;

    pthread_mutex_lock(&f->assignedRunway->mtx);
    f->assignedRunway->isOccupied = true;
    f->assignedRunway->currentThread = pthread_self();

    pthread_mutex_lock(&coutMutex);
    cout << GREEN << "[" << formatTimestamp(globalsimMins) << "][DEPARTURE][" << f->type << "] "
         << f->formattedFlightNumber() << " from "
         << f->assignedRunway->name << RESET << endl;
    pthread_mutex_unlock(&coutMutex);

    checkPhase(f, "AtGateDep", 0, 5);
    checkPhase(f, "TaxiDep", 15, 30);
    simulateFaultHandling(f);
    checkPhase(f, "TakeoffRoll", 200, 290);
    checkPhase(f, "Climb", 250, 463);
    checkPhase(f, "Cruise", 800, 900);

    f->assignedRunway->isOccupied = false;
    pthread_mutex_unlock(&f->assignedRunway->mtx);

    pthread_mutex_lock(&coutMutex);
    cout << GREEN << "[COMPLETE] " << f->formattedFlightNumber()
         << " off " << f->assignedRunway->name << RESET << endl
         << CYAN << "------------------------------------------------\n"
         << RESET;
    pthread_mutex_unlock(&coutMutex);

    return nullptr;
}

class AirportVisualizer {
    private:
        sf::RenderWindow window;
        sf::Font font;
        std::vector<Flight*>& flights;
        Runway& runwayA;
        Runway& runwayB;
        Runway& runwayC;
        
        // Assets
        sf::Texture airportTexture;
        sf::Texture planeTexture;
        sf::Sprite airportSprite;
        std::map<int, sf::Sprite> planeSprites;
        
    public:
        AirportVisualizer(std::vector<Flight*>& flts, Runway& rwA, Runway& rwB, Runway& rwC)
            : flights(flts), runwayA(rwA), runwayB(rwB), runwayC(rwC) {
            
            window.create(sf::VideoMode(1200, 800), "Airport Traffic Control Simulator");
            window.setFramerateLimit(60);
            
            // Load font
            if (!font.loadFromFile("arial.ttf")) {
                std::cout << "Failed to load font!" << std::endl;
            }
            
            // Load textures
            if (!airportTexture.loadFromFile("airport.png")) {
                // Create a default texture if file not found
                airportTexture.create(800, 600);
                sf::Image img;
                img.create(800, 600, sf::Color(50, 100, 50));
                airportTexture.update(img);
            }
            
            if (!planeTexture.loadFromFile("plane.png")) {
                // Create a default plane texture
                planeTexture.create(32, 32);
                sf::Image img;
                img.create(32, 32, sf::Color::White);
                planeTexture.update(img);
            }
            
            airportSprite.setTexture(airportTexture);
            airportSprite.setPosition(200, 100);
        }
        
        void update() {
            // Update plane sprites based on flight positions
            for (auto& flight : flights) {
                if (planeSprites.find(flight->flightNumber) == planeSprites.end()) {
                    // Create new sprite for this flight
                    sf::Sprite sprite;
                    sprite.setTexture(planeTexture);
                    sprite.setScale(0.5f, 0.5f);
                    planeSprites[flight->flightNumber] = sprite;
                }
                
                // Position the plane based on its coordinates and phase
                sf::Sprite& sprite = planeSprites[flight->flightNumber];
                
                // Map flight coordinates to screen coordinates
                float x = 600 + (flight->longitude / MAX_EAST_BOUNDARY) * 400;
                float y = 400 - (flight->latitude / MAX_NORTH_BOUNDARY) * 300;
                
                // Adjust for altitude
                float scale = 0.3f + (flight->altitude / MAX_ALTITUDE) * 0.3f;
                sprite.setScale(scale, scale);
                
                // Set color based on flight type
                if (flight->type == "Emergency") {
                    sprite.setColor(sf::Color::Red);
                } 
                else if (flight->type == "Cargo") {
                    sprite.setColor(sf::Color::Yellow);
                } 
                else {
                    sprite.setColor(sf::Color::White);
                }
                
                sprite.setPosition(x, y);
                
                // Rotate based on direction
                if (flight->direction == "North") {
                    sprite.setRotation(0);
                } 
                else if (flight->direction == "South") {
                    sprite.setRotation(180);
                } 
                else if (flight->direction == "East") {
                    sprite.setRotation(90);
                } 
                else { // West
                    sprite.setRotation(270);
                }
            }
        }
        
        
        void render() {
            window.clear(sf::Color(20, 30, 40));
            // Draw airport background
            window.draw(airportSprite);
            
            // Draw runways
            drawRunway(runwayA, 300, 300, 400, 50);
            drawRunway(runwayB, 300, 400, 400, 50);
            drawRunway(runwayC, 500, 300, 50, 400);
            
            // Draw planes
            for (const auto& pair : planeSprites) {
                window.draw(pair.second);
            }
            
            // Draw flight info
            drawFlightInfo();
            
            window.display();
        }
        
        void drawRunway(Runway& runway, float x, float y, float width, float height) {
            sf::RectangleShape rect(sf::Vector2f(width, height));
            rect.setPosition(x, y);
            
            // Color based on runway status
            if (runway.isOccupied) {
                rect.setFillColor(sf::Color(200, 50, 50, 200));
            } else {
                rect.setFillColor(sf::Color(50, 50, 50, 200));
            }
            
            rect.setOutlineColor(sf::Color::White);
            rect.setOutlineThickness(2);
            
            // Draw runway name
            sf::Text text;
            text.setFont(font);
            text.setString(runway.name);
            text.setCharacterSize(14);
            text.setFillColor(sf::Color::White);
            text.setPosition(x + width/2 - 20, y + height/2 - 10);
            
            window.draw(rect);
            window.draw(text);
        }
        
        void drawFlightInfo() {
            sf::RectangleShape infoPanel(sf::Vector2f(300, 700));
            infoPanel.setPosition(880, 50);
            infoPanel.setFillColor(sf::Color(30, 30, 30, 220));
            window.draw(infoPanel);
            
            sf::Text headerText;
            headerText.setFont(font);
            headerText.setString("Flight Information");
            headerText.setCharacterSize(20);
            headerText.setStyle(sf::Text::Bold);
            headerText.setFillColor(sf::Color::White);
            headerText.setPosition(900, 60);
            window.draw(headerText);
            
            float y = 100;
            for (auto& flight : flights) {
                if (y > 700) break; // Don't overflow the panel
                
                sf::Text flightText;
                flightText.setFont(font);
                std::string info = flight->formattedFlightNumber() + " | " + 
                                   flight->type + " | " + flight->phase + 
                                   " | " + std::to_string(int(flight->currentSpeed)) + " km/h";
                flightText.setString(info);
                flightText.setCharacterSize(12);
                
                // Color based on flight type
                if (flight->type == "Emergency") {
                    flightText.setFillColor(sf::Color::Red);
                } 
                else if (flight->avnActivated) {
                    flightText.setFillColor(sf::Color::Yellow);
                }
                else {
                    flightText.setFillColor(sf::Color::White);
                }
                
                flightText.setPosition(900, y);
                window.draw(flightText);
                y += 20;
            }
        }
        
        bool isOpen() {
            return window.isOpen();
        }
        
        void processEvents() {
            sf::Event event;
            while (window.pollEvent(event)) {
                if (event.type == sf::Event::Closed) {
                    window.close();
                }
            }
        }
    };

class Scheduler {
private:
    vector<Airline> airlines;
    vector<Flight*> userFlights;
    vector<Flight*> allFlights;
    vector<pthread_t> threads;
    AirportVisualizer* visualizer = nullptr;
    int simDuration;
    int currentTime_ = 0;

    int nextNorth=0, nextSouth=0, nextEast=0, nextWest=0;
    
    const int EST_EMERGENCY_TIME = 5;
    const int EST_CARGO_TIME = 8;
    const int EST_COMMERCIAL_TIME = 10;

public:
    Scheduler(int minutes, vector<Flight*>& flights): simDuration(minutes) {
        airlines = {
            {"PIA", "Commercial", 6, 4},
            {"AirBlue", "Commercial", 4, 4},
            {"FedEx", "Cargo", 3, 2},
            {"Pakistan Airforce","Military", 2, 1},
            {"Blue Dart", "Cargo", 2, 2},
            {"AghaKhan Air", "Medical", 2, 1}
        };
        
        for(auto& flight : flights){
            this->userFlights.push_back(flight);
            allFlights.push_back(flight);
        }
    }

    void setVisualizer(AirportVisualizer* vis) {
        visualizer = vis;
    }

    std::vector<Flight*>& getAllFlights(){
        return allFlights;
    }
    void run() {
        for (int t = 0; t <= simDuration; ++t) {
            currentTime_ = t;

            pthread_mutex_lock(&timeMutex);
            globalsimMins = t;
            pthread_mutex_unlock(&timeMutex);

            logTime(t);
            generateFlights(t);
            dispatchFlights();
            
            // ADDED: Update estimated wait times
            updateWaitTimes();

            pthread_mutex_lock(&coutMutex);
            cout << "\n    --- Minute " << t << " Summary ---\n";
            cout << " FLT  |   PHASE   | SPEED |   STATUS   | WAIT EST\n"; 
            cout << "------+-----------+-------+------------+---------\n";
            for (Flight* f : allFlights) {
                cout << " " 
                     << f->formattedFlightNumber() 
                     << " | " << setw(9) << f->phase
                     << " | " << setw(5) << int(f->currentSpeed)
                     << " | " << setw(10) << f->status
                     << " | " << setw(5) << f->estimatedWaitTime << " min" 
                     << "\n";
            }
            cout << "---------------------------------------\n" << RESET;
            pthread_mutex_unlock(&coutMutex);

            if (visualizer) {
                sf::Clock clock;
                while (clock.getElapsedTime().asSeconds() < 10.0f && visualizer->isOpen()) {
                    visualizer->processEvents();
                    visualizer->update();
                    visualizer->render();
                    sf::sleep(sf::milliseconds(16)); // ~60 FPS
                }
            }
            else {
                sleep(10);
            }
        }
        joinAll();
    }

private:

    void updateWaitTimes() {
        pthread_mutex_lock(&queueMutex);
        
        // Process arrival queues
        for (int p = 1; p < maxPriority; ++p) {
            int position = 0;
            int waitEstimate = 0;
            
            queue<Flight*> tempQueue = arrivalQueues[p];
            while (!tempQueue.empty()) {
                Flight* f = tempQueue.front();
                tempQueue.pop();
                
               
                if (p == 1) { // Emergency
                    waitEstimate = position * EST_EMERGENCY_TIME;
                } 
                else if (p == 2) { // Cargo
                    // Add estimated time for any emergencies
                    int emergencyCount = arrivalQueues[1].size();
                    waitEstimate = (emergencyCount * EST_EMERGENCY_TIME) + (position * EST_CARGO_TIME);
                } 
                else { // Commercial
                    // Add estimated time for emergencies and cargo
                    int emergencyCount = arrivalQueues[1].size();
                    int cargoCount = arrivalQueues[2].size();
                    waitEstimate = (emergencyCount * EST_EMERGENCY_TIME) + (cargoCount * EST_CARGO_TIME) + (position * EST_COMMERCIAL_TIME);
                }
                
                f->estimatedWaitTime = waitEstimate;
                position++;
            }
        }
        
        // Process departure queues (similar logic)
        for (int p = 1; p < maxPriority; ++p) {
            int position = 0;
            int waitEstimate = 0;
            
            queue<Flight*> tempQueue = departureQueues[p];
            while (!tempQueue.empty()) {
                Flight* f = tempQueue.front();
                tempQueue.pop();
                
                if (p == 1) { // Emergency
                    waitEstimate = position * EST_EMERGENCY_TIME;
                } else if (p == 2) { // Cargo
                    int emergencyCount = departureQueues[1].size();
                    waitEstimate = (emergencyCount * EST_EMERGENCY_TIME) + (position * EST_CARGO_TIME);
                } else { // Commercial
                    int emergencyCount = departureQueues[1].size();
                    int cargoCount = departureQueues[2].size();
                    waitEstimate = (emergencyCount * EST_EMERGENCY_TIME) + 
                                    (cargoCount * EST_CARGO_TIME) +
                                    (position * EST_COMMERCIAL_TIME);
                }
                
                f->estimatedWaitTime = waitEstimate;
                position++;
            }
        }
        
        pthread_mutex_unlock(&queueMutex);
    }

    void logTime(int t) {
        pthread_mutex_lock(&coutMutex);
        cout << YELLOW << "\n====== [TIME: " << t << " min] ======\n"
             << RESET;
        int totArr=0, totDep=0;
        pthread_mutex_lock(&queueMutex);
        for(int p=1; p<maxPriority; ++p){
          totArr += arrivalQueues[p].size();
          totDep += departureQueues[p].size();
        }
        pthread_mutex_unlock(&queueMutex);
        cout << CYAN << "[QUEUE] Arr: " << totArr
             << " | Dep: " << totDep << RESET << endl;
        pthread_mutex_unlock(&coutMutex);
    }

    Airline* findAirline(const string& name) {
        for (auto &a : airlines) if (a.name==name) return &a;
        return nullptr;
    }

    void generateFlights(int t) {
        pthread_mutex_lock(&queueMutex);
        if (t%3==0) spawnFlight("North","PIA","Arrival",10);
        if (t%2==0) spawnFlight("South","AirBlue","Arrival",5);
        if (t%2==0) spawnFlight("East","AirBlue","Departure",15);
        if (t%4==0) spawnFlight("West","Blue Dart","Departure",20);
        pthread_mutex_unlock(&queueMutex);
    }

    void spawnFlight(const string& dir, const string& airline, const string& category, int emergencyChance) {
        Airline* al = findAirline(airline);
        pthread_mutex_lock(&airlineMutex);
        bool ok = (al != nullptr && al->canLaunch());
        pthread_mutex_unlock(&airlineMutex);
        if (!ok) {
            pthread_mutex_lock(&coutMutex);
            cout << RED << "[DENIED] " << airline << " no slots\n" << RESET;
            pthread_mutex_unlock(&coutMutex);
            return;
        }

        static int fc = 100;
        fc++;

        Flight* f = new Flight;
        f->flightNumber = fc;
        f->airlineName = airline;
        f->category = category;
        f->direction = dir;

        int rnd = getRandomInt(1, 100);
        if (rnd <= emergencyChance) {
            f->type = "Emergency";
        } 
        else {
            if (airline == "FedEx" || airline == "Blue Dart") {
                f->type = "Cargo";
            } 
            else {
                f->type = "Commercial";
            }
        }

        f->status = "Waiting";
        f->enqueueTime = currentTime_;
  
        if (f->type == "Emergency") {
            f->estimatedWaitTime = 0;  // Emergency flights get priority
        } 
        else if (f->type == "Cargo") {
            f->estimatedWaitTime = 5; 
        } 
        else {
            f->estimatedWaitTime = 10; 
        }
        
        allFlights.push_back(f);

        int pr = f->priorityLevel();
        if (category == "Arrival") {
            arrivalQueues[pr].push(f);
        } 
        else {
            departureQueues[pr].push(f);
        }

        pthread_mutex_lock(&coutMutex);
        cout << GREEN << "[NEW] " << f->formattedFlightNumber() << " (" << f->type << " " << category << ") from "<< dir 
             << " | Est. Wait: " << f->estimatedWaitTime << " min" << RESET << endl;  // ADDED: Display estimated wait
        pthread_mutex_unlock(&coutMutex);
    }

    void dispatchFlights() {
    pthread_mutex_lock(&queueMutex);

    // Arrival
    Flight* f = nullptr;
    for (int p = 1; p < maxPriority; ++p) {
        if (!arrivalQueues[p].empty()) {
            f = arrivalQueues[p].front();
            arrivalQueues[p].pop();
            break;
        }
    }

    if (f != nullptr) {
        if (f->direction == "North" || f->direction == "South") {
            f->assignedRunway = &runwayA;
        } 
        else {
            f->assignedRunway = &runwayC;
        }

        bool doLaunch = true;
        if (pthread_mutex_trylock(&f->assignedRunway->mtx) != 0) {
            // Runway busy, handle based on priority
            if (f->priorityLevel() == 1) {
                // Emergency flight - implement strong preemption of current flight
                pthread_mutex_lock(&coutMutex);
                cout << RED << "[EMERGENCY PREEMPTION] " << f->formattedFlightNumber() 
                     << " preempting runway " << f->assignedRunway->name << RESET << endl;
                pthread_mutex_unlock(&coutMutex);
                
                // Find flight using the runway and forcefully preempt it
                for (Flight* other : allFlights) {
                    if (other->assignedRunway == f->assignedRunway && 
                        other->status != "Complete" && other->priorityLevel() > 1) {
                        
                        // First try cooperative yielding
                        other->yield = true;
                        other->preempted = true;
                        
                        pthread_mutex_lock(&coutMutex);
                        cout << YELLOW << "[PREEMPTING] Flight " << other->formattedFlightNumber() << RESET << endl;
                        pthread_mutex_unlock(&coutMutex);
                        
                        // Allow a brief moment for cooperative yielding
                        usleep(100000); 
                        
                        // If cooperative approach doesn't work, use pthread_cancel for forced preemption
                        if (pthread_mutex_trylock(&f->assignedRunway->mtx) != 0) {
                            pthread_mutex_lock(&coutMutex);
                            cout << RED << "[FORCED CANCEL] Cancelling thread for flight " 
                                 << other->formattedFlightNumber() << RESET << endl;
                            pthread_mutex_unlock(&coutMutex);
                            
                            // Cancel the thread (forceful preemption)
                            pthread_cancel(other->flightThread);
                            
                            // Wait a moment for the cancellation to take effect
                            usleep(200000); // 200ms
                            
                            // Mark the runway as available
                            f->assignedRunway->isOccupied = false;
                            pthread_mutex_unlock(&f->assignedRunway->mtx);
                            
                            // Try again to lock the runway
                            if (pthread_mutex_trylock(&f->assignedRunway->mtx) != 0) {
                                // If still can't get the lock, re-queue 
                                arrivalQueues[f->priorityLevel()].push(f);
                                doLaunch = false;
                                
                                pthread_mutex_lock(&coutMutex);
                                cout << YELLOW << "[REQUEUE FRONT] Emergency " << f->formattedFlightNumber() 
                                     << " still waiting for runway " << f->assignedRunway->name << RESET << endl;
                                pthread_mutex_unlock(&coutMutex);
                            }
                        }
                        break;
                    }
                }
            } 
            else {
                // Non-emergency, re-queue (peechay)
                arrivalQueues[f->priorityLevel()].push(f);
                doLaunch = false;
                
                pthread_mutex_lock(&coutMutex);
                cout << CYAN << "[REQUEUE BACK] " << f->formattedFlightNumber() 
                     << " waiting for runway " << f->assignedRunway->name << RESET << endl;
                pthread_mutex_unlock(&coutMutex);
            }
        }
        
        if (doLaunch) {
            pthread_mutex_unlock(&f->assignedRunway->mtx); // Will lock again in flight thread
            pthread_t t;
            
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
            
            pthread_create(&t, &attr, simulateArrivalFlight, f);
            pthread_attr_destroy(&attr);
            
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
            
            threads.push_back(t);
            f->flightThread = t;
        }
    }

    f = nullptr;
    for (int p = 1; p < maxPriority; ++p) {
        if (!departureQueues[p].empty()) {
            f = departureQueues[p].front();
            departureQueues[p].pop();
            break;
        }
    }

    if (f != nullptr) {
        if (f->type == "Cargo") {
            f->assignedRunway = &runwayC;
        } else {
            f->assignedRunway = &runwayB;
        }
        
        bool doLaunch = true;
        if (pthread_mutex_trylock(&f->assignedRunway->mtx) != 0) {
            // Runway busy, handle based on priority
            if (f->priorityLevel() == 1) {
                // Emergency flight - implement strong preemption
                pthread_mutex_lock(&coutMutex);
                cout << RED << "[EMERGENCY PREEMPTION] " << f->formattedFlightNumber() 
                     << " preempting runway " << f->assignedRunway->name << RESET << endl;
                pthread_mutex_unlock(&coutMutex);
                
                // Find flight using the runway and forcefully preempt it
                for (Flight* other : allFlights) {
                    if (other->assignedRunway == f->assignedRunway && 
                        other->status != "Complete" && other->priorityLevel() > 1) {
                        
                        // First try cooperative yielding
                        other->yield = true;
                        other->preempted = true;
                        
                        pthread_mutex_lock(&coutMutex);
                        cout << YELLOW << "[PREEMPTING] Flight " << other->formattedFlightNumber() 
                             << " with thread ID " << other->flightThread << RESET << endl;
                        pthread_mutex_unlock(&coutMutex);
                        
                        // Allow a brief moment for cooperative yielding
                        usleep(100000); 
                        
                        // If cooperative approach doesn't work, use pthread_cancel
                        if (pthread_mutex_trylock(&f->assignedRunway->mtx) != 0) {
                            pthread_mutex_lock(&coutMutex);
                            cout << RED << "[FORCED CANCEL] Cancelling thread for flight " 
                                 << other->formattedFlightNumber() << RESET << endl;
                            pthread_mutex_unlock(&coutMutex);
                            
                            // Cancel the thread (forceful preemption)
                            pthread_cancel(other->flightThread);
                            
                            // Wait a moment for the cancellation to take effect
                            usleep(200000); // 200ms
                            
                            // Mark the runway as available
                            f->assignedRunway->isOccupied = false;
                            pthread_mutex_unlock(&f->assignedRunway->mtx);
                            
                            // Try again to lock the runway
                            if (pthread_mutex_trylock(&f->assignedRunway->mtx) != 0) {
                                // If still can't get the lock, re-queue
                                departureQueues[f->priorityLevel()].push(f);
                                doLaunch = false;
                                
                                pthread_mutex_lock(&coutMutex);
                                cout << YELLOW << "[REQUEUE FRONT] Emergency " << f->formattedFlightNumber() 
                                     << " still waiting for runway " << f->assignedRunway->name << RESET << endl;
                                pthread_mutex_unlock(&coutMutex);
                            }
                        }
                        break;
                    }
                }
            } 
            else {
                // Non-emergency, re-queue (peechay pt2)
                departureQueues[f->priorityLevel()].push(f);
                doLaunch = false;
                
                pthread_mutex_lock(&coutMutex);
                cout << CYAN << "[REQUEUE BACK] " << f->formattedFlightNumber() 
                     << " waiting for runway " << f->assignedRunway->name << RESET << endl;
                pthread_mutex_unlock(&coutMutex);
            }
        }
        
        if (doLaunch) {
            pthread_mutex_unlock(&f->assignedRunway->mtx); // Will lock again in flight thread
            pthread_t t;

            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
            
            pthread_create(&t, &attr, simulateDepartureFlight, f);
            pthread_attr_destroy(&attr);
            
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
            
            threads.push_back(t);
            f->flightThread = t;
        }
    }

    pthread_mutex_unlock(&queueMutex);
}

    void joinAll() {
        for (auto& t : threads) {
            pthread_join(t, nullptr);
        }
        threads.clear();
        
        for (auto& f : allFlights) {
            if (f != nullptr) {
                delete f;
            }
        }
        allFlights.clear();
    }
};

void loadFromConsole(vector<Flight*>& userFlights, int simDuration) {
    cout << "How many flights? ";
    int N; cin >> N;
    for (int i = 0; i < N; ++i) {
        Flight* f = new Flight();
        cout << "\nFlight #" << (i+1) << ":\n";
        cout << "  Number (int): ";          cin >> f->flightNumber;
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        cout << "  Airline (e.g. PIA): ";     getline(cin, f->airlineName);
        cout << "  Type (Commercial/Cargo/Emergency): ";  cin >> f->type;
        cout << "  Category (Arrival/Departure): ";       cin >> f->category;
        cout << "  Direction (North/South/East/West): ";    cin >> f->direction;
        cout << "  Scheduled minute [0–" << simDuration << "]: ";
        cin >> f->enqueueTime;
        f->status = "Waiting";
        userFlights.push_back(f);
    }
}

void loadFromFile(vector<Flight*>& userFlights, int simDuration) {
    string filename;
    cout << "Enter filename\n";
    cout << ">";
    cin  >> filename;

    ifstream in(filename);
    if (!in) {
        cout << "Failed to open " << filename << "\n";
        return;
    }
    while (!in.eof()) {
        Flight* f = new Flight();
        in >> f->flightNumber
           >> f->airlineName
           >> f->type
           >> f->category
           >> f->direction
           >> f->enqueueTime;
        if (!in){ 
            delete f; 
            break; 
        }
        if (f->enqueueTime < 0 || f->enqueueTime > simDuration) {
            cout << RED << "[SKIP] invalid time " << f->enqueueTime << RESET << "\n";
            delete f;
            continue;
        }
        f->status = "Waiting";
        userFlights.push_back(f);
    }
}


int main(int argc, char* argv[]) {
    srand(time(0));

    pthread_mutex_init(&coutMutex, nullptr);
    pthread_mutex_init(&airlineMutex, nullptr);
    pthread_mutex_init(&queueMutex, nullptr);
    pthread_mutex_init(&timeMutex, nullptr);
  
    mkfifo(FIFO_SIM2ATC, 0666);

    int simMins = 5;
    if (argc > 1) {
        simMins = atoi(argv[1]);
        if (simMins <= 0) simMins = 60;
    }
    if (argc > 2 && strcmp(argv[2], "quiet") == 0) {
        disctOutput = false;
    }
    
    int mode;
    cout << "1) Use hardcoded flights\n";
    cout << "2) Enter flights manually\n";
    cout << "> ";
    cin >> mode;


    vector<Flight*> userFlights;

    if (mode == 2) {
        char sub;
        cout << "  a) Console input\n";
        cout << "  b) From text file in current directory\n";
        cout << "> ";
        cin >> sub;

        if (sub == 'a') {
            loadFromConsole(userFlights, simMins);
        } 
        else {
            loadFromFile(userFlights, simMins);
        }
    }
   
    Scheduler scheduler(simMins, userFlights);
    
    AirportVisualizer visualizer(scheduler.getAllFlights(), runwayA, runwayB, runwayC);
    scheduler.setVisualizer(&visualizer);
    
    cout << BOLD << "\n=== Airport Traffic Control Simulator ===" << RESET << endl;
    
    scheduler.run();

    scheduler.run();
    
    pthread_mutex_destroy(&coutMutex);
    pthread_mutex_destroy(&airlineMutex);
    pthread_mutex_destroy(&queueMutex);
    pthread_mutex_destroy(&timeMutex);
    
    cout << BOLD << "\n=== Simulation Complete ===" << RESET << endl;
    
    return 0;
}

