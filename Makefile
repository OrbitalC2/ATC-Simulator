# Makefile

# Compiler and flags
CXX        := g++
CXXFLAGS   := -std=c++17 -pthread -Wall -Wextra

# SFML linker flags (for the simulation target)
SFML_LIBS  := -lsfml-graphics -lsfml-window -lsfml-system

# List of all services to build
SERVICES   := simulation avn_generator airline_portal stripe_payment

# Default target: build everything
all: $(SERVICES)

# Build rule for 'simulation' (includes SFML libraries)
simulation: simulation.cpp shared_data.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(SFML_LIBS)

# Build rule for 'avn_generator'
avn_generator: avn_generator.cpp shared_data.h
	$(CXX) $(CXXFLAGS) $< -o $@

# Build rule for 'airline_portal'
airline_portal: airline_portal.cpp shared_data.h
	$(CXX) $(CXXFLAGS) $< -o $@

# Build rule for 'stripe_payment'
stripe_payment: stripe_payment.cpp shared_data.h
	$(CXX) $(CXXFLAGS) $< -o $@

.PHONY: run clean

# Launch all services in separate gnome-terminal windows
run: all
	@echo "Launching all services in new terminal windows..."
	gnome-terminal --window --title="Simulation"    -- bash -c "./simulation; exec bash" &
	gnome-terminal --window --title="AVN Generator"  -- bash -c "./avn_generator; exec bash" &
	gnome-terminal --window --title="Airline Portal" -- bash -c "./airline_portal; exec bash" &
	gnome-terminal --window --title="StripePay"      -- bash -c "./stripe_payment; exec bash" &

# Remove all built executables
clean:
	rm -f $(SERVICES)
