from ros2topic.verb.hz import HzVerb
from ros2hermos._bridged_ros2cli import add_bridge_arguments
from ros2hermos._bridged_ros2cli import spawn_bridge_and_run


class TopicHzHermosVerb(HzVerb):
    """Measure DDS bridge reception frequency; use stats_hermos for native counters."""

    def add_arguments(self, parser, cli_name):
        super().add_arguments(parser, cli_name)
        add_bridge_arguments(parser)

    def main(self, *, args):
        print('Measuring DDS bridge reception frequency. Native counters: ros2 topic stats_hermos.')
        return spawn_bridge_and_run(
            args, topic_name=args.topic_name, action_fn=lambda a: HzVerb.main(self, args=a))
