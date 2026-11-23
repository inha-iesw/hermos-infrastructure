"""Bridge plugin generation remains independent of the upper runtime exporter."""

import argparse

import em
import pytest

from ros2hermos.verb.hermos_generate_bridge_plugins import GenerateBridgePluginsVerb, InterfaceType


def test_bridge_generation_has_no_exporter_dependency_or_sources(tmp_path, monkeypatch):
    monkeypatch.setattr(em.Interpreter, '_wasProxyInstalled', False)
    generator = GenerateBridgePluginsVerb()
    monkeypatch.setattr(generator, '_get_ros2hermos_version', lambda: '2.3.5')
    generator._generate_package(str(tmp_path), ['std_msgs/msg/String'],
                                ['example_interfaces/srv/AddTwoInts'])
    assert {path.name for path in (tmp_path / 'src').iterdir()} == {
        'generic_functions.hpp', 'generic_functions.cpp',
        'pubsub_bridge_plugin_std_msgs_msg_String.cpp',
        'service_bridge_plugin_example_interfaces_srv_AddTwoInts.cpp',
    }
    cmake = (tmp_path / 'CMakeLists.txt').read_text()
    manifest = (tmp_path / 'package.xml').read_text()
    assert 'project(hermos_bridge_plugins)' in cmake
    assert '<name>hermos_bridge_plugins</name>' in manifest
    assert 'hermos_runtime' not in cmake + manifest
    assert 'dds_export' not in cmake + manifest


def test_bridge_cli_does_not_offer_exporter_option():
    parser = argparse.ArgumentParser()
    GenerateBridgePluginsVerb().add_arguments(parser, 'generate_bridge_plugins')
    with pytest.raises(SystemExit) as error:
        parser.parse_args(['--dds-export', '--message-types', 'std_msgs/msg/String'])
    assert error.value.code == 2


@pytest.mark.parametrize('type_name', [
    '/msg/String', 'std_msgs/msg/', '../msg/String', 'std_msgs/msg/../String',
    'std_msgs;other/msg/String', 'std_msgs/msg/String;Other',
    'std_msgs)/msg/String', 'std_msgs/msg/String)', 'std_msgs/msg/String\nOther',
    'StdMsgs/msg/String', 'std__msgs/msg/String', 'std_msgs_/msg/String',
    'std_msgs/msg/lowercase', 'std_msgs/msg/With_Underscore',
])
def test_generator_rejects_invalid_identifiers(type_name):
    generator = GenerateBridgePluginsVerb()
    assert generator._validate_types([type_name], InterfaceType.MESSAGE) == []


@pytest.mark.parametrize('type_name,kind', [
    ('std_msgs/msg/String', InterfaceType.MESSAGE),
    ('example_interfaces/srv/AddTwoInts', InterfaceType.SERVICE),
    ('test_msgs/msg/UInt64', InterfaceType.MESSAGE),
])
def test_generator_preserves_valid_ros_identifiers(type_name, kind):
    assert GenerateBridgePluginsVerb()._validate_types([type_name], kind) == [type_name]
