#ifndef COMPILER_H
#define COMPILER_H

// Inkluderingsskydd
#define RUNSPACE_COMPILER_ERROR -1

// Funktionsprototyp för en viktig funktion från compiler.c
// Denna funktion kan användas av andra moduler för att exekvera externa kommandon
int execute_command(const char *command, int *status);

#endif // COMPILER_H
