from setuptools import find_packages, setup

package_name = 'ros2hermos'

setup(
    name=package_name,
    version='2.3.5',
    packages=find_packages(),
    data_files=[
        ('share/' + package_name, ['package.xml']),
        ('share/ament_index/resource_index/packages',
            ['resource/' + package_name]),
    ],
    package_data={
        'ros2hermos.templates': [
            '*.em',
        ],
        'ros2hermos.static_sources': [
            '*.hpp',
            '*.cpp',
        ],
    },
    extras_require={'test': ['pytest']},
    entry_points={
        'ros2cli.command': [
            'hermos = ros2hermos.command.hermos:HermosCommand',
        ],
        'ros2hermos.verb': [
            'bridge-daemon-status = ros2hermos.verb.hermos_bridge_daemon_status:BridgeDaemonStatusVerb',
            'generate-bridge-plugins = ros2hermos.verb.hermos_generate_bridge_plugins:GenerateBridgePluginsVerb',
            'version = ros2hermos.verb.hermos_version:VersionVerb',
            'discovery-daemon-status = ros2hermos.verb.hermos_discovery_daemon_status:DiscoveryDaemonStatusVerb',
        ],
        'ros2topic.verb': [
            'list_hermos = ros2hermos.verb.topic_list_hermos:ListHermosVerb',
            'info_hermos = ros2hermos.verb.topic_info_hermos:TopicInfoHermosVerb',
            'hz_hermos = ros2hermos.verb.topic_hz_hermos:TopicHzHermosVerb',
            'echo_hermos = ros2hermos.verb.topic_echo_hermos:TopicEchoHermosVerb',
            'delay_hermos = ros2hermos.verb.topic_delay_hermos:TopicDelayHermosVerb',
        ],
        'ros2node.verb': [
            'list_hermos = ros2hermos.verb.node_list_hermos:ListHermosVerb',
            'info_hermos = ros2hermos.verb.node_info_hermos:NodeInfoHermosVerb',
        ],
        'ros2bag.verb': [
            'record_hermos = ros2hermos.verb.bag_record_hermos:BagRecordHermosVerb',
        ],
    },
)
