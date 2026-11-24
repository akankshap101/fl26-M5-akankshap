# M5 design

**M4 baseline.** 
No code fixes. The uploaded baseline passed the local GUI tests. 

**Threads and shared data.** 
The GUI thread owns the workspace, ProcessingCore, the state object, and the ID of the build it is waiting for. BuildController`= creates one worker on the first build request and reuses it until it exits. The worker builds from a workspace and sends its result to the GUI thread through a queued callback.

The controller mutex protects pending_, active_token_, and stopping_. Each request has its own atomic cancellation flag. The GUI is set on cancellation, and the worker checks it while building. The worker waits for the ready_ condition variable to request or shutdown. When exit is choosen, the controller clears pending work, cancels active work, wakes the worker, and joins it. The IPC receiver uses its own atomic stop flag and queues document additions on the GUI thread..

States: 
Empty: No documents. Add goes to Dirty.
Dirty: Workspace needs a build. Build goes to Building.
Building: A request is pending or running. A matching success goes to Current and failure goes to Failed. 
Current: The installed corpus matches the workspace. Build goes to Building.
Failed: The error is visible and Build retries through Building.

A successful document add moves the workspace to Dirty. Reset clears the workspace and moves it to Empty. Exit shuts down the threads from any state.

**Event trace.**
On the GUI thread,start_build() submits build A. The worker takes A from pending_ in run() and releases the mutex before building. Then remote add reaches add_document_file() on the GUI thread. The state handler calls cancel_build(), which clears the build ID and cancels A. The workspace then moves to Dirty.Then the user starts to build B. The worker checks A's cancellation flag and then takes B. If A's completion reaches the GUI, finish_build() checks the result ID against the wanted ID. Only B's matching result can install the new corpus.

**Decisions.**
Some decisions made is build is disabled while the workspace is in Building. Adding or resetting documents invalidates that request and lets the user request a replacement once documents are available. Remote adds use the same add_document_file() path as local adds and one pending slot is enough because only the latest valid workspace snapshot needs to be built.

**Faults.**
AIWS_FAULT_NO_CANCEL is used to disable the cancellation check in BuildChunker::check_cancelled(). Reset_cancels_build_before_replacement catches the delay. AIWS_FAULT_SILENT_SHUTDOWN prevents an idle worker from waking during stop_and_join(). exit_joins_idle_worker catches the resulting timeout.

**AI disclosure.** 
ChatGPT was used to help understand project and develop an outline
