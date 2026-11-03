from ros2cli.command import add_subparsers_on_demand
from ros2cli.command import CommandExtension


class HermosCommand(CommandExtension):
    """Hermos related sub-commands."""

    def add_arguments(self, parser, cli_name):
        self._subparser = parser
        parser.add_argument(
            '--version', '-v',
            action='store_true',
            default=False,
            help='Show version information for Hermos components',
        )
        add_subparsers_on_demand(
            parser,
            cli_name,
            '_verb',
            'ros2hermos.verb',
            required=False,
        )

    def main(self, *, parser, args):
        if args.version:
            from ros2hermos.verb.hermos_version import VersionVerb
            return VersionVerb().main(args=args)

        if not hasattr(args, '_verb'):
            self._subparser.print_help()
            return 0

        extension = getattr(args, '_verb')
        return extension.main(args=args)
