#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <fcntl.h>      
#include <unistd.h>     
#include <cstring>
#include <sys/stat.h>   
#include "shared_data.h"

using namespace std;

class AirlinePortal {
    map<string, AirspaceViolationNotice> activeAVNs;
    vector<AirspaceViolationNotice> historicalAVNs;

    int fdInAVN;       
    int fdOutStripe;  
    int fdInStripe;    

    const string USER = "admin";
    const string PASS = "letmein";

public:
  
    AirlinePortal() {
        mkfifo(FIFO_AVN2PORT, 0666);
        mkfifo(FIFO_STRIPE2PORT, 0666);
        mkfifo(FIFO_AVN2STRIPE, 0666); 
        
        //non-blocking so multiple avns can be displayed
        fdInAVN = open(FIFO_AVN2PORT,   O_RDONLY | O_NONBLOCK);
        fdInStripe = open(FIFO_STRIPE2PORT,O_RDONLY | O_NONBLOCK);
        fdOutStripe= open(FIFO_AVN2STRIPE, O_WRONLY);
        if (fdInAVN<0 || fdInStripe<0 || fdOutStripe <0) {
            perror("AirlinePortal: open FIFO");
            exit(1);
        }
    }


    ~AirlinePortal(){
        close(fdInAVN);
        close(fdOutStripe);
        close(fdInStripe);
    }

    bool login(){
        string u,p;
        cout<<"Username\n> ";     
        cin>>u;
        cout<<"Password\n> ";     
        cin>>p;
        return (u == USER && p == PASS);
    }

    
    void run() {

        while (!login()) {
            cout<<"Login failed. Try again.\n";
        }
        cout<<"Login successful.\n";

        for (;;) {

            drainAVNs();
            drainPayments();
    

            cout<<"\n--- Airline Portal ---\n"
                <<"1) View Active AVNs\n"
                <<"2) View History\n"
                <<"3) Pay an AVN\n"
                <<"4) Refresh (Check for new AVNs)\n"
                <<"5) Exit\n"
                <<"(Note: Select 'Refresh' to check for newly arrived AVNs)\n"
                <<"> ";
            int choice;
            if (!(cin>>choice)) break;
    
            switch (choice) {
                case 1: viewActive();  break;
                case 2: viewHistory(); break;
                case 3: payAVN(); break;
                case 4: 
                    cout<<"Checking for new AVNs...\n"; 
                    drainAVNs();
                    drainPayments();
                    break;
                case 5:
                    return;
                default:
                    cout<<"Invalid choice\n";
            }
    
            sleep(1);
        }
    }


private:
    void viewActive(){
        cout<<"\n=== Active AVNs ===\n";
        for(auto const& p: activeAVNs){
        auto const& a = p.second;
        cout<<"ID: "<<a.avnID <<" | Flight: "<<a.flightNumber <<" | Status: "<<a.paymentStatus<<"\n";
        }
    }

    void viewHistory(){
    cout<<"\n=== Historical AVNs ===\n";
    for(auto const& a: historicalAVNs){
        cout<<"ID: "<<a.avnID <<" | Flight: "<<a.flightNumber <<" | Status: "<<a.paymentStatus<<"\n";
        }
    }

    void drainAVNs() {
        AirspaceViolationNotice avn;
        ssize_t n;
        while ((n = read(fdInAVN, &avn, sizeof(avn))) == sizeof(avn)) {
            string key{avn.avnID};
            activeAVNs[key] = avn;
            cout<<"\n[Portal] New AVN: "<<avn.avnID<<"\n";
        }
        if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) {
            perror("AirlinePortal: read AVN");
        }
    }

    void drainPayments() {
        PaymentInfo pay;
        ssize_t n;
        while ((n = read(fdInStripe, &pay, sizeof(pay))) == sizeof(pay)) {
            // cout << "[Portal] New AVN: " << pay.avnID << ", total amount: " << pay.amountDue << "\n";
            auto it = activeAVNs.find(pay.avnID);
            if (it != activeAVNs.end()) {
                if (pay.paymentSuccessful) {
                    // mark paid
                    strncpy(it->second.paymentStatus, "paid",
                            sizeof(it->second.paymentStatus)-1);
                    it->second.paymentStatus[
                      sizeof(it->second.paymentStatus)-1] = '\0';
    
                    historicalAVNs.push_back(it->second);
                    activeAVNs.erase(it);
                    cout<<"\n[Portal] Payment confirmed for "<<pay.avnID<<"\n";
                } 
                else {
    
                    cout<<"\n[Portal] Payment FAILED for "<<pay.avnID
                        <<" - Amount paid: "<<pay.amountPaid
                        <<", Amount due: "<<pay.amountDue<<"\n";
                }
            }
        }
        if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) {
            perror("AirlinePortal: read payment");
        }
    }


    void payAVN() {
        // Make sure we have the latest AVNs and payment statuses
        drainAVNs();
        drainPayments();
        
        cout << "Enter AVN ID: ";
        char id[32] = {}; 
        cin >> id;

        auto it = activeAVNs.find(id);
        if (it == activeAVNs.end()) {
            cout << "No such active AVN.\n";
            return;
        }
        
        cout << "\n=== Amount Due: $" << it->second.totalAmount << " ===\n\n";
        
        cout << "Amount to pay (" << it->second.totalAmount << "): ";
        double amt; 
        cin >> amt;
        
        PaymentInfo req{};
     
        strncpy(req.avnID, id, sizeof(req.avnID)-1);
        req.avnID[sizeof(req.avnID)-1] = '\0';
     
        snprintf(req.aircraftID, sizeof(req.aircraftID), "F%04d", it->second.flightNumber);
    
        strncpy(req.aircraftType, it->second.aircraftType, sizeof(req.aircraftType)-1);
        req.aircraftType[sizeof(req.aircraftType)-1] = '\0';
    
        req.amountDue = it->second.totalAmount;
        req.amountPaid = amt;
        req.paymentSuccessful = false;
    
        req.paymentDateTime[0] = '\0';
    
        if (write(fdOutStripe, &req, sizeof(req)) == sizeof(req)) {
            cout << "Payment request sent for " << id << "\n";
        } else {
            perror("AirlinePortal: write payment req");
        }
    }
};

int main(){
    AirlinePortal portal;
    portal.run();
    return 0;
}

