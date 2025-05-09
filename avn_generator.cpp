#include <iostream>
#include <map>
#include <fcntl.h>      
#include <unistd.h>     
#include <sys/stat.h> 
#include <cstring>  
#include "shared_data.h"

using namespace std;

class AVNGenerator {
    map<string,AirspaceViolationNotice> avnDatabase;
    int fdIn;   
    int fdPort;    
    int fdStripe;  

public:
    AVNGenerator(){
        mkfifo(FIFO_SIM2ATC,  0666);
        mkfifo(FIFO_AVN2PORT, 0666);
        mkfifo(FIFO_AVN2STRIPE,0666);

       
        fdIn = open(FIFO_SIM2ATC,   O_RDONLY);
        fdPort = open(FIFO_AVN2PORT,  O_WRONLY);
        fdStripe = open(FIFO_AVN2STRIPE,O_WRONLY);

        if (fdIn<0 || fdPort<0 || fdStripe<0) {
            perror("AVNGenerator: open FIFO");
            exit(1);
        }
    }

    ~AVNGenerator() {
        close(fdIn);
        close(fdPort);
        close(fdStripe);
    }

    void run() {
        AirspaceViolationNotice avn;
        while (true) {
            ssize_t n = read(fdIn, &avn, sizeof(avn));
            if (n == sizeof(avn)) {
                avnDatabase[avn.avnID] = avn;
                cout << "[AVN] Received AVN# " << avn.avnID << " for Flight " << avn.flightNumber << "\n";

                // 2) Forward the full AVN to the Airline Portal
                if (write(fdPort, &avn, sizeof(avn)) == sizeof(avn)) {
                    cout << "[AVN] Forwarded to AirlinePortal\n";
                } 
                else {
                    perror("AVNGenerator: write to AirlinePortal");
                }

                //payment to sedn to stripe
                PaymentInfo pay{};
              
                strncpy(pay.avnID, avn.avnID, sizeof(pay.avnID)-1);
                pay.avnID[sizeof(pay.avnID)-1] = '\0';

                snprintf(pay.aircraftID, sizeof(pay.aircraftID), "F%04d", avn.flightNumber);

                strncpy(pay.aircraftType, avn.aircraftType, sizeof(pay.aircraftType)-1);
                pay.aircraftType[sizeof(pay.aircraftType)-1] = '\0';

                pay.amountDue = avn.totalAmount;
                pay.amountPaid = 0.0;
                pay.paymentSuccessful= false;
                // cout << "[AVN] Creating payment info for " << avn.avnID << ", amount due: " << avn.totalAmount << "\n";

                // format current time
                {
                auto now = std::chrono::system_clock::now();
                auto t   = std::chrono::system_clock::to_time_t(now);
                strftime(pay.paymentDateTime, sizeof(pay.paymentDateTime), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                }

                if (write(fdStripe, &pay, sizeof(pay)) != sizeof(pay)) {
                    perror("AVNGenerator: write to StripePay");
                }
                else {
                    cout << "[AVN] Sent payment info to StripePay\n";
                }


            }
            else if (n < 0) {
                perror("AVNGenerator: read");
            }
            sleep(1);
        }
    }
};

int main(){
    AVNGenerator gen;
    gen.run();
    return 0;
}
