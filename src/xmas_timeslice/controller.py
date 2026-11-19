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

        camera_base_ip = options.get("base_ip", "192.168.4")
        camera_admin_password = options.get("admin_pass", "change-me-ota")
        self.image_dir = options.get("image_dir", "test/images")

        self.cameras = [
            Camera(
                num=i,
                base_ip=camera_base_ip,
                image_dir=self.image_dir,
                admin_pass=camera_admin_password,
            )
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
                        "auto_disarm": (
                            lambda cam=camera: cam.idle_s,
                            lambda v, cam=camera: cam.set_config({"idle_s": v}),
                            {
                                "description": "Number of seconds the camera remains Armed. 0 will disable the auto-disarm functionality. Setting this value will reboot the camera to take effect."
                            },
                        ),
                        "exposure_lock": (
                            lambda cam=camera: cam.exposure_locked,
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
                        ),
                    },
                }
                for camera in self.cameras
            },
            "control": {
                "arm_all": (
                    lambda: all(
                        [
                            cam.shot_status.get("state", "disconencted")
                            in ["armed", "arming", "capturing"]
                            for cam in self.cameras
                        ]
                    ),
                    self.arm_all,
                    {"description": "Arm or disarm all the connected cameras"},
                ),
                "lock_all": (
                    lambda: all([cam.exposure_locked for cam in self.cameras]),
                    self.lock_all,
                    {"description": "Lock or unlock all the camera exposures"}
                ),
                "trigger_all": (
                    None,
                    lambda _: self.trigger_all(),
                    {"description": "Take a picture with all the cameras, using the software trigger"}
                )
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

    def arm_all(self, arm: bool):
        """Arm or disarm all the connected cameras."""
        for cam in self.cameras:
            cam.arm(arm)

    def lock_all(self, lock: bool):
        """Lock or Unlock exposure on all connected cameras."""
        for cam in self.cameras:
            cam.lock_exposure(lock)

    def trigger_all(self):
        """Use the software trigger to trigger all cameras to take a picture."""
        for cam in self.cameras:
            cam.trigger()
