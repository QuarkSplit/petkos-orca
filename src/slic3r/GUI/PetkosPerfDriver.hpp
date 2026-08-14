#ifndef slic3r_GUI_PetkosPerfDriver_hpp_
#define slic3r_GUI_PetkosPerfDriver_hpp_

namespace Slic3r { namespace GUI {

//Petko's Orca: the scripted half of the latency instrument.
//
//The plate board is custom-painted and the 3D canvas is a GL surface, so no UI-automation
//tool can reach either; and a number taken by a human dragging a mouse cannot be compared
//with the same number taken a day later. This drives the same production paths the user's
//clicks reach - camera orbit, select_plate, set_plate_printer, board mouse events - from the
//event loop, at a plate count given on the command line, and writes the result out.
//
//Set PETKOS_PERF_SCRIPT to a comma-separated spec; inert when unset. Keys:
//  plates=N     grow the project to N plates, each carrying one cube (default: leave alone)
//  warmup=N     frames rendered and discarded before timing starts (default 60)
//  orbit=N      frames of camera orbit to time (default 600)
//  switch=N     plate switches to time (default 10)
//  assign=N     printer assignments to time (default 4)
//  printer=NAME printer preset to assign; default is the first installed one that is not
//               the project printer
//  board=N      synthetic mouse moves across the plate board (default 200)
//  drag=N       synthetic board drags (default 5)
//  quit=0|1     close the app when the run finishes (default 1)
//
//Requires PETKOS_PERF=1 as well; the driver refuses to run without the instrument, because
//a scripted run that records nothing looks exactly like a fast one.
void petkos_perf_driver_start();

//PetkosOrca: the acceptance run. Inert unless PETKOS_ACCEPT is set. Drives one real job all the
//way through - a model split across two plates on two different printers in two materials, sliced
//and saved - using nothing but the application's own APIs.
void petkos_acceptance_start();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PetkosPerfDriver_hpp_
