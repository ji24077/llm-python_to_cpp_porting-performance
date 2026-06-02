import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from markdown_it import MarkdownIt


def main() -> None:
    """Read Markdown from stdin and render CommonMark HTML to stdout."""
    source = sys.stdin.read()
    renderer = MarkdownIt("commonmark")
    result = renderer.render(source)
    sys.stdout.write(result)


if __name__ == "__main__":
    main()
