Airport Traffic Control Simulator (ATCS)
========================================

ATCS is a **console-first**, POSIX-compliant mini-ecosystem that models day-to-day
operations at a three-runway airport.  
It was written to practise / demonstrate:

* **Thread scheduling & synchronisation** (pthreads, mutexes, condition vars)
* **Inter-process communication** (named pipes/FIFOs)
* **Producer–consumer patterns** across multiple binaries
* **Graceful cancellation & resource cleanup** in long-running C++ code
* Proof-of-concept graphics with **SFML** (optional, _not_ a full UI)

------------------------------------------------------------
High-level flow
------------------------------------------------------------

1. **simulation**  
   • Spawns flights from a manifest or random generator  
   • Uses three runway queues, prioritising emergencies  
   • Monitors altitude, speed and airspace boundaries  
   • Emits an *AVN* (Air-space Violation Notice) to `/tmp/avn_pipe`

2. **avn_generator**  
   • Reads `/tmp/avn_pipe`, assigns fine + due date  
   • Broadcasts details to `/tmp/stripe_pipe` and `/tmp/portal_pipe`

3. **stripe_payment**  
   • Emulates Stripe REST flow but via FIFO messages  
   • Keeps a CSV ledger of pending / successful payments in `fines.csv`

4. **airline_portal**  
   • CLI for ops staff: list active AVNs, view history, pay fines  
   • Colourised output; locks the screen while waiting for Stripe reply

*(If SFML headers and libs are present, `simulation` also opens a tiny window
that shows planes taxiing, taking off, and landing. Omit SFML and the build
drops seamlessly to headless mode.)*

------------------------------------------------------------
Repository layout
------------------------------------------------------------
src/
  simulation.cpp        – core engine + (optional) visualiser
  avn_generator.cpp     – AVN dispatcher
  stripe_payment.cpp    – mock payment processor
  airline_portal.cpp    – airline UI
  shared_data.h         – common structs, FIFO paths, enum values
  Makefile
assets/                 – 32 × 32 sprites + font (only for SFML)
flights_full.txt        – demo flight schedule (200 flights, 12 h)

------------------------------------------------------------
Build
------------------------------------------------------------
sudo apt install g++ make libsfml-dev      # sfml optional
make                                       # builds all four binaries

# skip graphics
make NOSFML=1

------------------------------------------------------------
Run – quick demo
------------------------------------------------------------
Terminal 1$  ./simulation 30 quiet          # 30 simulated minutes, terse logging
Terminal 2$  ./avn_generator
Terminal 3$  ./stripe_payment
Terminal 4$  ./airline_portal

> In the simulator choose “file input” and point it at flights_full.txt.

Typical portal session
----------------------
1) View Active AVNs        2) Pay Fine        3) History        4) Quit
choice> 1
ID  FLIGHT      OFFENCE          FINE (PKR)   DUE
12  THY287      Altitude >4000   575 000       23-Jun-25 14:05
…

choice> 2
Enter AVN ID> 12
Processing… ✓  Payment accepted

------------------------------------------------------------
Configuration flags
------------------------------------------------------------
simulation <minutes> <quiet|verbose|debug>
    minutes   – stop after N simulated minutes (real-time ≈ 1 s/min)
    quiet     – minimal console
avn_generator [-l logfile]
stripe_payment [-fail N]    # auto-reject every Nth payment to test retries
airline_portal has no flags

All FIFO files are auto-created under /tmp and deleted on SIGINT.

------------------------------------------------------------
Extending the project
------------------------------------------------------------
* Swap FIFOs for ZeroMQ or a light HTTP gateway
* Add weather disruptions that dynamically close runways
* Integrate a real payment sandbox (Stripe test API)
* Replace SFML with a web front-end served via websocket

------------------------------------------------------------
License
------------------------------------------------------------
MIT – do whatever you want, just keep the notice.

