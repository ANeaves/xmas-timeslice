"""Adapter Module for Xmas Timeslice."""

import logging

from odin_control.adapters.adapter import ApiAdapter, ApiAdapterResponse
from odin_control.adapters.util import require_controller, response_types
from tornado.httputil import HTTPServerRequest

from ._version import __version__
from .controller import XmasTimesliceController, XmasTimesliceError


class XmasTimesliceAdapter(ApiAdapter):
    """Xmas Timeslice Adapter class inheriting base adapter functionality."""

    controller_cls = XmasTimesliceController
    error_cls = XmasTimesliceError
    version = __version__

    @response_types("application/json", "application/vnd.odin-native", "application/octet-stream", "image/jpeg", default="application/json")
    @require_controller
    def get(self, path: str, request: HTTPServerRequest):
        """Handle a HTTP Get request, with overwridden handling for non-json requests."""
        stripped_path = path.strip("/")
        if stripped_path.startswith("data"):
            try:
                splitpath = path.split("/", 1)
                rest = splitpath[1] if len(splitpath) > 1 else ""
                logging.debug(f"GET request on special data path: {rest}")
                ctrl: XmasTimesliceController = self.controller

                response, datatype = ctrl.get_data(rest)
                match datatype:
                    case "jpeg":
                        content_type = "image/jpeg"
                    case _:
                        content_type = "application/octet-stream"
                status_code = 200
            except self.error_cls as err:
                response = {"error": str(err)}
                status_code = 400
                content_type = "application/json"
            return ApiAdapterResponse(response, content_type=content_type, status_code=status_code)
        else:
            return super().get(path, request)
