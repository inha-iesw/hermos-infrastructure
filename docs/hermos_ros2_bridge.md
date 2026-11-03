# Bridge Internal Design

This document covers internal design notes for the Hermos-ROS 2 Bridge. For user-facing documentation (bridge modes, configuration, plugins, QoS behavior), see [Hermos-ROS 2 Bridge](https://autowarefoundation.github.io/agnocast_doc/migration-guide/bridge/) on the documentation site.

## Isolation & Safety

Because a single bridge manager process is shared across all Hermos processes in the IPC namespace, a crash in the bridge manager will affect all bridged topics.

## Internal Structure

Each bridge direction creates a pair of internal publisher and subscriber.
The internal publisher's QoS is fixed to maximize compatibility, ensuring connectivity regardless of the external QoS settings.

**R2A Bridge (RosToHermosBridge)**:

```mermaid
flowchart LR
   classDef listNode text-align:left,stroke-width:1px;

    ExtPub[External ROS 2 Publisher]

    subgraph Bridge [R2A Bridge]
        direction TB

        R2Sub["<b>ROS 2 Subscription</b><br/>-QoS: <br/> inherited from Hermos subscriber"]:::listNode

        AgPub["<b>Hermos Publisher</b><br/>- Depth: 10 (DEFAULT_QOS_DEPTH)<br/>- Durability: TransientLocal"]:::listNode

        R2Sub -->|callback| AgPub
    end

    AgSub[External Hermos Subscriber]

    ExtPub --> R2Sub
    AgPub --> AgSub
```

**A2R Bridge (HermosToRosBridge)**:

```mermaid
flowchart LR
   classDef listNode text-align:left,stroke-width:1px;

    AgPubExt[External Hermos Publisher]

    subgraph Bridge [A2R Bridge]
        direction TB

        AgSub["<b>Hermos Subscription</b><br/>- QoS: <br/> inherited from Hermos publisher"]:::listNode

        R2Pub["<b>ROS 2 Publisher</b><br/>- Depth: 10 (DEFAULT_QOS_DEPTH)<br/>- Reliability: Reliable<br/>- Durability: TransientLocal"]:::listNode

        AgSub -->|callback| R2Pub
    end

    ExtSub[External ROS 2 Subscriber]

    AgPubExt --> AgSub
    R2Pub --> ExtSub
```
