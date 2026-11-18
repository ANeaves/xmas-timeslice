"""Controller Module for Xmas Timeslice."""

import logging
from os import path
from odin_control.adapters.parameter_tree import ParameterTree, ParameterTreeError
from odin_control.adapters.base_controller import BaseController, BaseError

from xmas_timeslice.camera import Camera, CameraError


class XmasTimesliceError(BaseError):
    """Simple exception class to wrap lower-level exceptions."""


class XmasTimesliceController(BaseController):
    """Controller class for Xmas Timeslice."""

    def __init__(self, options: dict[str, str]):
        """Initialize the controller with configuration options.

        :param options: Dictionary containing configuration options for the controller.
        """
        self.options = options

        self.camera_base_ip = options.get("base_ip", "192.168.4")
        self.image_dir = options.get("image_dir", "test/images")

        self.cameras = [
            Camera(num=i + 1, base_ip=self.camera_base_ip, image_dir=self.image_dir)
            for i in range(8)
        ]

        tree = {
            "cams": {
                f"camera_{camera.number}": {
                    "status": (lambda cam=camera: cam.shot_status, None),
                    "control": {
                        "arm": (
                            None,
                            lambda v, cam=camera: cam.arm(v),
                            {"description": "Arm or disarm the camera"},
                        ),
                        "exposure_lock": (
                            lambda cam=camera: cam.shot_status.get("locked", False),
                            lambda v, cam=camera: cam.lock_exposure(v),
                            {
                                "description": "Lock or unlock the camera's Exposue, White Balance, and Gain. This can speed up camera arming."
                            },
                        ),
                        "trigger": (
                            None,
                            lambda _, cam=camera: cam.trigger(),
                            {
                                "description": "Use the software trigger to take an image."
                            },
                        )
                    },
                }
                for camera in self.cameras
            },
        }
        self.param_tree = ParameterTree(tree)

    def get_data(self, path: str):
        """Get non-json data, such as images or other binary objects."""
        try:
            splitpath = path.split("/", 1)
            match splitpath[0]:
                case "image":
                    return self.get_image(int(splitpath[1])), "jpeg"
                case _:
                    raise XmasTimesliceError(f"Unknown Data Path: {path}")
        except Exception as err:
            # TODO: more explicit error handling
            raise XmasTimesliceError(err)

    def get_image(self, cam_num: int):
        """Return the image data from the camera."""
        try:
            data = self.cameras[cam_num - 1].get_image()
            return data
        except CameraError as err:
            raise XmasTimesliceError(err)

    def initialize(self, adapters):
        """Initialize the controller with access to other adapters.

        This method is called after all adapters have been loaded and allows
        the controller to establish inter-adapter communication if needed.

        :param adapters: Dictionary mapping adapter names to adapter instances.
        """
        self.adapters = adapters
        logging.debug(f"Adapters initialized: {list(adapters.keys())}")
        # Add to param tree if needed post-initialization

    def cleanup(self):
        """Clean up controller resources and state.

        This method should be implemented to properly clean up any resources,
        close connections, or perform other shutdown tasks when the controller
        is being stopped or destroyed.
        """
        logging.info("Cleaning up XmasTimesliceController")
        for camera in self.cameras:
            camera.cleanup()

    def get(self, path, with_metadata=False):
        """Get data from the controller at the specified path.

        :param path: The path string identifying the resource to retrieve.
        :param with_metadata: Whether to include metadata in the response.

        :return: The requested data, format depends on controller implementation.
        """
        logging.debug(f"Adapter GET: {path}")
        try:
            return self.param_tree.get(path, with_metadata)
        except (ParameterTreeError, CameraError) as error:
            logging.error(error)
            raise XmasTimesliceError(error)

    def set(self, path, data):
        """Set data in the controller at the specified path.

        :param path: The path string identifying the resource to modify.
        :param data: The data to set at the specified path.
        """
        try:
            self.param_tree.set(path, data)
        except (ParameterTreeError, CameraError) as error:
            logging.error(error)
            raise XmasTimesliceError(error)
