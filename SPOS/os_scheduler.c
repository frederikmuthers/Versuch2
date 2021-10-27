#include "os_scheduler.h"
#include "util.h"
#include "os_input.h"
#include "os_scheduling_strategies.h"
#include "os_taskman.h"
#include "os_core.h"
#include "lcd.h"

#include <avr/interrupt.h>

//----------------------------------------------------------------------------
// Private Types
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// Globals
//----------------------------------------------------------------------------

//! Array of states for every possible process
Process os_processes[MAX_NUMBER_OF_PROCESSES];

//! Array of function pointers for every registered program
Program *os_programs[MAX_NUMBER_OF_PROGRAMS];

//! Index of process that is currently executed (default: idle)
ProcessID currentProc;

//----------------------------------------------------------------------------
// Private variables
//----------------------------------------------------------------------------

//! Currently active scheduling strategy
SchedulingStrategy currentSchedulingStrategy;

//! Count of currently nested critical sections
uint8_t criticalSectionCount;

//! Used to auto-execute programs.
uint16_t os_autostart;

//----------------------------------------------------------------------------
// Private function declarations
//----------------------------------------------------------------------------

//! ISR for timer compare match (scheduler)
ISR(TIMER2_COMPA_vect) __attribute__((naked));

//----------------------------------------------------------------------------
// Function definitions
//----------------------------------------------------------------------------

/*!
 *  Timer interrupt that implements our scheduler. Execution of the running
 *  process is suspended and the context saved to the stack. Then the periphery
 *  is scanned for any input events. If everything is in order, the next process
 *  for execution is derived with an exchangeable strategy. Finally the
 *  scheduler restores the next process for execution and releases control over
 *  the processor to that process.
 */
ISR(TIMER2_COMPA_vect) {
    //sichere Laufzeikontext
	saveContext();
	
	//sichere Stackpointer des Prozesses
	os_processes[os_getCurrentProc()].sp = SP;
	
	//lade Scheduler Stack in das SP Register
	SP = BOTTOM_OF_ISR_STACK;
	
	//aktueller Prozess geht von running auf ready
	os_processes[os_getCurrentProc()].state = OS_PS_READY;
	
	//Asuwahl des n�chsten prozesses je nach Schedule Strategy
	if(currentSchedulingStrategy == OS_SS_EVEN) {
		currentProc = os_Scheduler_Even(os_processes, currentProc);
	} 
	else if(currentSchedulingStrategy == OS_SS_RANDOM){
		currentProc = os_Scheduler_Random(os_processes, currentProc);
	} 
	else if(currentSchedulingStrategy == OS_SS_ROUND_ROBIN){
		currentProc = os_Scheduler_RoundRobin(os_processes, currentProc);
		
	} 
	else if(currentSchedulingStrategy == OS_SS_INACTIVE_AGING){
		currentProc = os_Scheduler_InactiveAging(os_processes, currentProc);
	} 
	//Nur noch Run to completion is �brig
	else{
		currentProc = os_Scheduler_RunToCompletion(os_processes, currentProc);
	}
	
	//fortzuf�hrender Prozess geht auf running
	os_processes[os_getCurrentProc()].state = OS_PS_RUNNING;
	
	//stackpointer f�r fortzuf�hrenden Prozess wiederherstellen
	SP = os_processes[os_getCurrentProc()].sp;
	
	//Laufzeitkontext des fortzuf�hrenden Prozesses wird wiederhergestellt
	restoreContext();
}

/*!
 *  Used to register a function as program. On success the program is written to
 *  the first free slot within the os_programs array (if the program is not yet
 *  registered) and the index is returned. On failure, INVALID_PROGRAM is returned.
 *  Note, that this function is not used to register the idle program.
 *
 *  \param program The function you want to register.
 *  \return The index of the newly registered program.
 */
ProgramID os_registerProgram(Program* program) {
    ProgramID slot = 0;

    // Find first free place to put our program
    while (os_programs[slot] &&
           os_programs[slot] != program && slot < MAX_NUMBER_OF_PROGRAMS) {
        slot++;
    }

    if (slot >= MAX_NUMBER_OF_PROGRAMS) {
        return INVALID_PROGRAM;
    }

    os_programs[slot] = program;
    return slot;
}

/*!
 *  Used to check whether a certain program ID is to be automatically executed at
 *  system start.
 *
 *  \param programID The program to be checked.
 *  \return True if the program with the specified ID is to be auto started.
 */
bool os_checkAutostartProgram(ProgramID programID) {
    return !!(os_autostart & (1 << programID));
}

/*!
 *  This is the idle program. The idle process owns all the memory
 *  and processor time no other process wants to have.
 */
PROGRAM(0, AUTOSTART) {
    while(1){
		lcd_writeString(".\n");
		delayMs(DEFAULT_OUTPUT_DELAY);
	}
}

/*!
 * Lookup the main function of a program with id "programID".
 *
 * \param programID The id of the program to be looked up.
 * \return The pointer to the according function, or NULL if programID is invalid.
 */
Program* os_lookupProgramFunction(ProgramID programID) {
    // Return NULL if the index is out of range
    if (programID >= MAX_NUMBER_OF_PROGRAMS) {
        return NULL;
    }

    return os_programs[programID];
}

/*!
 * Lookup the id of a program.
 *
 * \param program The function of the program you want to look up.
 * \return The id to the according slot, or INVALID_PROGRAM if program is invalid.
 */
ProgramID os_lookupProgramID(Program* program) {
    ProgramID i;

    // Search program array for a match
    for (i = 0; i < MAX_NUMBER_OF_PROGRAMS; i++) {
        if (os_programs[i] == program) {
            return i;
        }
    }

    // If no match was found return INVALID_PROGRAM
    return INVALID_PROGRAM;
}

/*!
 *  This function is used to execute a program that has been introduced with
 *  os_registerProgram.
 *  A stack will be provided if the process limit has not yet been reached.
 *  This function is multitasking safe. That means that programs can repost
 *  themselves, simulating TinyOS 2 scheduling (just kick off interrupts ;) ).
 *
 *  \param programID The program id of the program to start (index of os_programs).
 *  \param priority A priority ranging 0..255 for the new process:
 *                   - 0 means least favorable
 *                   - 255 means most favorable
 *                  Note that the priority may be ignored by certain scheduling
 *                  strategies.
 *  \return The index of the new process or INVALID_PROCESS as specified in
 *          defines.h on failure
 */
ProcessID os_exec(ProgramID programID, Priority priority) {
    //kritischer Bereich
	os_enterCriticalSection();
	for (ProcessID pid = 0 ; pid < MAX_NUMBER_OF_PROCESSES ; pid++){
		/*prozess state ist unused wenn
			a) gerade initialisiert
			b) state auf unused gesetzt
		*/
		if (os_processes[pid].state == OS_PS_UNUSED){
			//w�hle program aus mit hilfsfunktion
			Program *funktionszeiger = os_lookupProgramFunction(programID);
			//Nullpointer test
			if(funktionszeiger == NULL){
				//kritischen Bereich verlassen und Funktion beenden
				os_leaveCriticalSection();
				return INVALID_PROCESS;
			}else{
				//Prozesszustand, Priorit�t und ProgramID speichern
				os_processes[pid].state = OS_PS_READY;
				os_processes[pid].priority = priority;
				os_processes[pid].progID = programID;
				//Prozessstack vorbereiten
				StackPointer sp;
				//geh zum Boden des Stacks
				sp.as_int = PROCESS_STACK_BOTTOM(pid);
				
				//16 bit funktionszeiger als initiale R�cksrpungadresse speichern
				uint8_t lowbyte = (uint8_t) (funktionszeiger & 0x00ff);
				*(sp.as_ptr) = lowbyte;
				sp.as_int -= 1;
				uint8_t highbyte = (uint8_t) (funktionszeiger >> 8);
				*(sp.as_ptr) = highbyte;
				sp.as_int -= 1;
				
				//33 0 Bytes folgen. 1 f�r Statusregister (SREG) und 32 f�r Laufzeitkontext
				for (uint8_t i = 0 ; i < 33 ; i++){
					*(sp.as_ptr) = 0x00;
					sp.as_int -= 1;
				}
				
				//speichere Stackpointer im neuen Prozess
				os_processes[pid].sp = sp;
				
				//kritischen Bereich verlassen und Funktion beenden
				os_leaveCriticalSection();
				return pid;
			}
		}
	}
	//keine unbenutzen Prozessslots
	//kritischen Bereich verlassen und Funktion beenden
	os_leaveCriticalSection();
	return INVALID_PROCESS;
}

/*!
 *  If all processes have been registered for execution, the OS calls this
 *  function to start the idle program and the concurrent execution of the
 *  applications.
 */
void os_startScheduler(void) {
	currentProc = 0;
	os_processes[os_getCurrentProc()].state = OS_PS_RUNNING;
	PS = os_processes[os_getCurrentProc()].sp;
	restoreContext();
}

/*!
 *  In order for the Scheduler to work properly, it must have the chance to
 *  initialize its internal data-structures and register.
 */
void os_initScheduler(void) {
	//alle Prozessezust�nde werden auf unused gesetzt
    for(ProcessID pid = 0 ; pid < MAX_NUMBER_OF_PROCESSES ; pid++){
		os_processes[pid].state = OS_PS_UNUSED;
	}
	//jedes Program, was automatisch starten soll, wird ein Prozess zugeteilt
	for(ProgramID progID = 0 ; progID < MAX_NUMBER_OF_PROGRAMS ; progID++){
		if(os_checkAutostartProgram(progID)){
			os_exec(progID, DEFAULT_PRIORITY);
		}
	}
}

/*!
 *  A simple getter for the slot of a specific process.
 *
 *  \param pid The processID of the process to be handled
 *  \return A pointer to the memory of the process at position pid in the os_processes array.
 */
Process* os_getProcessSlot(ProcessID pid) {
    return os_processes + pid;
}

/*!
 *  A simple getter for the slot of a specific program.
 *
 *  \param programID The ProgramID of the process to be handled
 *  \return A pointer to the function pointer of the program at position programID in the os_programs array.
 */
Program** os_getProgramSlot(ProgramID programID) {
    return os_programs + programID;
}

/*!
 *  A simple getter to retrieve the currently active process.
 *
 *  \return The process id of the currently active process.
 */
ProcessID os_getCurrentProc(void) {
    return currentProc;
}

/*!
 *  This function return the the number of currently active process-slots.
 *
 *  \returns The number currently active (not unused) process-slots.
 */
uint8_t os_getNumberOfActiveProcs(void) {
    uint8_t num = 0;

    ProcessID i = 0;
    do {
        num += os_getProcessSlot(i)->state != OS_PS_UNUSED;
    } while (++i < MAX_NUMBER_OF_PROCESSES);

    return num;
}

/*!
 *  This function returns the number of currently registered programs.
 *
 *  \returns The amount of currently registered programs.
 */
uint8_t os_getNumberOfRegisteredPrograms(void) {
    uint8_t count = 0;
    for (ProcessID i = 0; i < MAX_NUMBER_OF_PROGRAMS; i++)
        if (*(os_getProgramSlot(i))) count++;
    // Note that this only works because programs cannot be unregistered.
    return count;
}

/*!
 *  Sets the current scheduling strategy.
 *
 *  \param strategy The strategy that will be used after the function finishes.
 */
void os_setSchedulingStrategy(SchedulingStrategy strategy) {
    currentSchedulingStrategy = strategy;
}

/*!
 *  This is a getter for retrieving the current scheduling strategy.
 *
 *  \return The current scheduling strategy.
 */
SchedulingStrategy os_getSchedulingStrategy(void) {
    return currentSchedulingStrategy;
}

/*!
 *  Enters a critical code section by disabling the scheduler if needed.
 *  This function stores the nesting depth of critical sections of the current
 *  process (e.g. if a function with a critical section is called from another
 *  critical section) to ensure correct behavior when leaving the section.
 *  This function supports up to 255 nested critical sections.
 */
void os_enterCriticalSection(void) {
	//speicher GIEB 
    uint8_t GlobalInterruptEnableBit = SREG & 0b10000000;
	
	//deaktiviere GIEB
	SREG &= 0b01111111; 
	
	//inkrementiere Verschatelungstiefe um 1
	criticalSectionCount++;
	
	//deaktiviere Scheduler mit OCIE2A Bit (1. Bit)
	TIMSK2 &= 0b11111101;
	
	//Wiederherstellung des gespeicherten GIEB
	SREG |= GlobalInterruptEnableBit;
}

/*!
 *  Leaves a critical code section by enabling the scheduler if needed.
 *  This function utilizes the nesting depth of critical sections
 *  stored by os_enterCriticalSection to check if the scheduler
 *  has to be reactivated.
 */
void os_leaveCriticalSection(void) {
    //speicher GIEB
    uint8_t GlobalInterruptEnableBit = SREG & 0b10000000;
    
    //deaktiviere GIEB
    SREG &= 0b01111111;
	
	//dekrementiere Verschaftelungstiefe um 1
	criticalSectionCount--;
	
	if(criticalSectionCount < 0){
		//Fehlermeldung, falls mehr Kritische Bereiche verlassen wurden als betreten wurden
		os_errorPStr("Zu oft os_leaveCriticalSection aufgerufen");
	} else if(criticalSectionCount == 0){
		//aktiviere Scheduler mit OCIE2A Bit (1. Bit) falls kein kritischer Bereich vorliegt
		TIMSK2 |= 0b00000010; 
	}
	
	//Wiederherstellung des gespeicherten GIEB
	SREG |= GlobalInterruptEnableBit;
}

/*!
 *  Calculates the checksum of the stack for a certain process.
 *
 *  \param pid The ID of the process for which the stack's checksum has to be calculated.
 *  \return The checksum of the pid'th stack.
 */
StackChecksum os_getStackChecksum(ProcessID pid) {
	StackPointer sp;
	StackChecksum sum;
	//vom Boden des Stacks anfangen
    sp.as_int = PROCESS_STACK_BOTTOM(pid);
	sum = *(sp.as_ptr);
	//bis zum Stackpointer durchlaufen und Bytes xor verkn�pfen
	while (sp.as_int > os_processes[pid].sp.as_int){
		sp.as_int--;
		sum = sum ^ *(sp.as_ptr);
	}
	return sum;
}
