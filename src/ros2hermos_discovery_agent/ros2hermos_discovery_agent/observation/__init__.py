"""Metadata-only factory for a supervisor-owned ROS context and executor."""

import os
import uuid

def create(context, args):
    """Create an unspun metadata node without claiming the bridge-agent slot.

    The caller must register an empty inventory and receive its supervisor's
    ACK before dispatching callbacks. It owns node/context cleanup and must
    terminate this process on failure. The supervisor owns duplicate-observer
    prevention; observation must not suppress a bridge-capable agent.
    No payload runtime or allocator is loaded.
    """
    if args:
        raise ValueError('metadata observation accepts no application arguments')
    # Keep protocol/snapshot imports independent of Node initialization and
    # avoid agent -> observation -> agent while retaining this factory entry.
    from ..agent import DiscoveryAgent
    return DiscoveryAgent(
        observation_only=True, context=context, exit_when_idle=False,
        session_id=os.environ.get('HERMOS_SESSION_ID') or str(uuid.uuid4()))
