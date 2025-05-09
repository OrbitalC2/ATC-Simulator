#include <iostream>
#include <map>
#include <fcntl.h>      
#include <unistd.h>    
#include <sys/stat.h> 
#include <chrono>
#include <ctime>
#include <cstring>
#include <sstream>
#include <iomanip>
#include "shared_data.h"

using namespace std;

class StripePay {
    map<string,PaymentInfo> pending;
    int fdIn;   
    int fdOut; 

public:
    StripePay() {
    
        mkfifo(FIFO_AVN2STRIPE, 0666);
        mkfifo(FIFO_STRIPE2PORT, 0666);

        fdIn = open(FIFO_AVN2STRIPE,  O_RDONLY);
        fdOut = open(FIFO_STRIPE2PORT, O_WRONLY);

        if (fdIn<0 || fdOut<0) {
            perror("StripePay: open FIFO");
            exit(1);
        }
        cout<<"[StripePay] Ready. Listening on "<<FIFO_AVN2STRIPE<<"\n";
    }

    ~StripePay(){
        close(fdIn);
        close(fdOut);
    }

    void run(){
        PaymentInfo msg;
        while(true){
            ssize_t n = read(fdIn, &msg, sizeof(msg));
            if (n != sizeof(msg)) {
                if (n<0) perror("StripePay: read");
                sleep(1);
                continue;
            }

            if (msg.amountPaid == 0) {
                pending[msg.avnID] = msg;
                cout<<"[StripePay] Registered AVN "<<msg.avnID<<" pending payment\n";
            }
            else {
                //payment request
                auto it = pending.find(msg.avnID);
                if (it == pending.end()) {
                    cerr<<"[StripePay] No pending AVN "<<msg.avnID<<"\n";
                } 
                else {
                    //successful payment
                    bool paymentSuccess = (msg.amountPaid >= it->second.amountDue);
                    if (paymentSuccess) {
           
                        msg.paymentSuccessful = true;
                        auto now = chrono::system_clock::now();
                        auto t   = chrono::system_clock::to_time_t(now);
                        stringstream ss;
                        ss<<put_time(localtime(&t), "%Y-%m-%d %H:%M:%S");
                        auto s = ss.str();
                        strncpy(msg.paymentDateTime, s.c_str(), sizeof(msg.paymentDateTime) - 1);
                        msg.paymentDateTime[sizeof(msg.paymentDateTime) - 1] = '\0';
                
                        // forward confirmation
                        write(fdOut, &msg, sizeof(msg));
                        cout<<"[StripePay] Confirmed payment for AVN "<<msg.avnID<<"\n";
                
                        pending.erase(it);
                    }
                    else {
                        // Mark payment as unsuccessful
                        msg.paymentSuccessful = false;
                        
                        auto now = chrono::system_clock::now();
                        auto t   = chrono::system_clock::to_time_t(now);
                        stringstream ss;
                        ss<<put_time(localtime(&t), "%Y-%m-%d %H:%M:%S");
                        auto s = ss.str();
                        strncpy(msg.paymentDateTime, s.c_str(), sizeof(msg.paymentDateTime) - 1);
                        msg.paymentDateTime[sizeof(msg.paymentDateTime) - 1] = '\0';
                        
                        // Send failure notification back to portal
                        write(fdOut, &msg, sizeof(msg));
                        cout<<"[StripePay] Insufficient amount for AVN "<<msg.avnID<<"\n";
 
                    }
                }
            }
        }
    }
};

int main(){
    StripePay svc;
    svc.run();
    return 0;
}
