# User todo list
Agent, do not implement these, it is for myself to keep track. If I ask you to add a task, keep it very clear and simple. Use simple language and do not make it overly verbose.

# Tasks
## Managing fault clearing and recovery states.
Clearing fault states, when do we want to do this, etc. Auto recovery. This also includes a path for interrupts to push the system into faulted.

## Stub out command structure
This will be command.c and commmand.h, with a single source of truth for reference, which is updated asynchronously using can and serial. Conflict management will not likely be an issue, but we will figure this out later.

## Check state needs to check for active system OCP and OVP faults.
Title explains it all.

## Timeouts, put them everywhere
type shit
