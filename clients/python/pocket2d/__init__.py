"""Python client for the Pocket2D agent protocol: a JSONL runtime client,
scenario files, a Gymnasium-style environment and a scripted-player session.
No third-party modules are required; gymnasium is used for spaces when
installed."""
from .runtime import ProtocolError, Runtime
from .scenario import Scenario, ScenarioError
from .env import PocketEnv, find_actor, read_property
from .session import Session
from .vector import VectorPocketEnv

__all__ = ["VectorPocketEnv", "PocketEnv", "ProtocolError", "Runtime", "Scenario", "ScenarioError", "Session", "find_actor", "read_property"]
