"""Adapter Module for Xmas Timeslice."""

from odin_control.adapters.adapter import ApiAdapter
from .controller import XmasTimesliceController, XmasTimesliceError
from ._version import __version__


class XmasTimesliceAdapter(ApiAdapter):
    """Xmas Timeslice Adapter class inheriting base adapter functionality."""

    controller_cls = XmasTimesliceController
    error_cls = XmasTimesliceError
    version = __version__
