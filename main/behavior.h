#pragma once
// State machine (behaviors.md §7) + call toggle (§4.6) + UI loop. Runs on core 1.
namespace behavior {
void start();   // creates the UI task (face ~30 fps, touch, servo policy)
}
