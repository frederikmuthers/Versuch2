#include "os_input.h"

#include <avr/io.h>
#include <stdint.h>

/*! \file

Everything that is necessary to get the input from the Buttons in a clean format.

*/

/*!
 *  A simple "Getter"-Function for the Buttons on the evaluation board.\n
 *
 *  \returns The state of the button(s) in the lower bits of the return value.\n
 *  example: 1 Button:  -pushed:   00000001
 *                      -released: 00000000
 *           4 Buttons: 1,3,4 -pushed: 000001101
 *
 */
/*
	Belegung des Ports C
	C0 Enter
	C1 Down
	C2 JTAG
	C3 JTAG
	C4 JTAG
	C5 JTAG
	C6 Up
	C7 ESC
*/
uint8_t os_getInput(void) {
	/*
	Alle Eingabebits müssen geflippt werden da Button gedrückt = 0 und Button nicht gedrückt = 1 im PIN Register ist
	Nicht an den vier Mittleren Pins interessiert, da diese nicht an Buttons angeschlossen sind
	*/
	uint8_t  a = ~(PINC) & 0b11000011;
	//extrahiere zwei höchsten Bits (Up und ESC)
	uint8_t b = a & 0b11000000;
	//shifte diese Bits an die Stelle Nummer 2 und 3
	b = (b >> 4);
	//Füge diese Bits der Rückgabe wieder hinzu
	a |= b;
	return a; 
}

/*!
 *  Initializes DDR and PORT for input
 */
void os_initInput() {
	//setze C0, C1, C6, C7 als Input, verändere andere Pins nicht
    DDRC &= 0b00111100;
	//setze Pullup Widerstände für C0, C1, C6, C7, verändere andere Pins nicht
	PORTC |= 0b11000011;
}

/*!
 *  Endless loop as long as at least one button is pressed.
 */
void os_waitForNoInput() {
    while(os_getInput() != 0){
		//warte
	}
}

/*!
 *  Endless loop until at least one button is pressed.
 */
void os_waitForInput() {
    while(os_getInput() == 0){
		//warte
	}
}
