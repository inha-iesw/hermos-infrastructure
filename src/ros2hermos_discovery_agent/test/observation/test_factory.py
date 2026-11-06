"""The supervisor owns dispatch; the metadata factory only creates endpoints."""

from types import SimpleNamespace
from unittest.mock import Mock

import pytest


def test_factory_rejects_application_arguments_before_initialization(monkeypatch):
    from ros2hermos_discovery_agent import observation, agent

    loader = Mock()
    monkeypatch.setattr(agent, 'load_query_library', loader)
    with pytest.raises(ValueError, match='arguments'):
        observation.create(object(), ['--export'])
    loader.assert_not_called()


def test_factory_does_not_claim_the_bridge_agent_slot(monkeypatch):
    from ros2hermos_discovery_agent import observation, agent

    library = SimpleNamespace(hermos_discovery_agent_register=Mock(return_value=1))
    node = Mock()
    monkeypatch.setattr(agent, 'load_query_library', lambda: library)
    monkeypatch.setattr(agent, 'DiscoveryAgent', node)
    assert observation.create(SimpleNamespace(get_domain_id=lambda: 42), []) is node.return_value
    library.hermos_discovery_agent_register.assert_not_called()


def test_factory_returns_unspun_metadata_node_in_supervisor_context(monkeypatch):
    from ros2hermos_discovery_agent import observation, agent

    library = SimpleNamespace(hermos_discovery_agent_register=Mock(return_value=0))
    node = Mock()
    context = SimpleNamespace(get_domain_id=lambda: 42)
    monkeypatch.setattr(agent, 'load_query_library', lambda: library)
    monkeypatch.setattr(agent, 'DiscoveryAgent', node)
    monkeypatch.setenv('ROS_DOMAIN_ID', '9')
    monkeypatch.setenv('HERMOS_SESSION_ID', 'epoch-a')
    assert observation.create(context, []) is node.return_value
    library.hermos_discovery_agent_register.assert_not_called()
    node.assert_called_once_with(
        observation_only=True, context=context, session_id='epoch-a', exit_when_idle=False)


def test_factory_uses_a_fresh_session_without_supervisor_environment(monkeypatch):
    from ros2hermos_discovery_agent import observation, agent

    library = SimpleNamespace(hermos_discovery_agent_register=Mock(return_value=0))
    node = Mock()
    monkeypatch.setattr(agent, 'load_query_library', lambda: library)
    monkeypatch.setattr(agent, 'DiscoveryAgent', node)
    monkeypatch.delenv('HERMOS_SESSION_ID', raising=False)
    observation.create(SimpleNamespace(get_domain_id=lambda: 42), [])
    first = node.call_args.kwargs['session_id']
    observation.create(SimpleNamespace(get_domain_id=lambda: 42), [])
    second = node.call_args.kwargs['session_id']
    assert first and second and first != second
