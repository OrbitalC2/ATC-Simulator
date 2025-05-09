#pragma once
#include <iostream>
#include <chrono>
#include <sstream>
using namespace std;

constexpr char FIFO_SIM2ATC[] = "/tmp/sim2atc";
constexpr char FIFO_ATC2AVN[] = "/tmp/atc2avn";
constexpr char FIFO_AVN2PORT[] = "/tmp/avn2portal";
constexpr char FIFO_AVN2STRIPE[] = "/tmp/avn2stripe";
constexpr char FIFO_STRIPE2PORT[]= "/tmp/stripe2portal";

struct AirspaceViolationNotice {
    char avnID[13];
    char airline[16];
    int flightNumber;
    char aircraftType[16];
    double recordedSpeed;
    double permissibleSpeed;
    char issuanceDateTime[20];
    double baseFine;
    double serviceFee;
    double totalAmount;
    char dueDate[20];
    char paymentStatus[8];

    static inline std::string generateAVNId(int flightNumber) {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count();
        std::ostringstream ss;
        ss << "AVN" << ms << "_" << flightNumber;
        return ss.str();
    }

    static double phaseMaxSpeed(const string& phase){
        if (phase == "Holding")      return 600.0;
        if (phase == "Approach")     return 290.0;
        if (phase == "Landing")      return 240.0;
        if (phase == "Taxi")         return 30.0;
        if (phase == "AtGate")       return 5.0;
        if (phase == "AtGateDep")    return 5.0;
        if (phase == "TaxiDep")      return 30.0;
        if (phase == "TakeoffRoll")  return 290.0;
        if (phase == "Climb")        return 463.0;
        if (phase == "Cruise")       return 900.0;
        return 0.0;  
    }
};

struct PaymentInfo {
    char avnID[32];
    char aircraftID[8];
    char aircraftType[16];
    double amountPaid;
    double amountDue;
    bool paymentSuccessful;
    char paymentDateTime[20];
};
