Tote Sentinel
A water-level monitor for three hydroponic reservoir totes (T1, T2, T3). One Waveshare ESP32-C6-LCD-1.3 board reads three HC-SR04 ultrasonic sensors, one mounted above each tote, and shows how many gallons are in each one.
How it works
Each HC-SR04 sits face-down above a tote (27 Gal. Tough Storage Bin Tote  https://www.homedepot.com/p/HDX-27-Gal-Tough-Storage-Tote-in-Black-and-Red-999-27G-HDX-RED/327528714). The sensor face is 13.5 in above the tote floor. The ESP32 sends a trigger pulse, the sensor sends out an ultrasonic ping, and the time the echo takes to come back tells the board the distance to the water surface. Subtract that distance from 13.5 in to get the water depth.
The totes have sloped sides, so depth isn't proportional to volume. Instead, the firmware converts depth to gallons with a calibration table. To build the table, each tote was filled a few gallons at a time and the depth was measured at each step. T1, T2 and T3 are identical 27-gallon totes, so the three tables were averaged into one table that covers all of them. The totes never hold more than 20 gallons in use, so that's where the table stops.
Hardware
• Waveshare ESP32-C6-LCD-1.3 (controller and display)
• 3 × HC-SR04 ultrasonic sensors
• Cat5 cable from the controller to each sensor, with RJ45 breakouts at both ends
• 1k/2k resistor dividers on each echo line, to bring the 5 V echo signal down to a level the ESP32 can safely read
• 47 µF capacitor at each sensor, 100 µF on the controller carrier board
• Protoboard carrier for the controller, and a 30 × 62 mm protoboard at each tote end with the HC-SR04 mounted underneath
Enclosure
The enclosure folder has 3D-printable STL files for the tote-end sensor housing.
Building
This is a PlatformIO project (Arduino framework, C++). Open the folder in VS Code with the PlatformIO extension installed, then build and upload to the board.
Calibration
If your totes are different, refill yours in steps of a few gallons, record the depth at each step, and replace the values in the calibration table in main.cpp.
