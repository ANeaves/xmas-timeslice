"""Module for the Camera Class, which connects to a single camera via HTTP for config and image copying."""

import logging
import time
from ipaddress import ip_address
from os import listdir, path as osPath
from typing import Any

import requests


class CameraError(Exception):
    """Exception class for Camera errors."""

    pass


STATUS_KEYS = [
    "state",
    "ip",
    "error",
    "missed",
    "shot_id",
    "size",
    "width",
    "height",
    "uptime_ms",
]


class Camera:
    """Camera Class, to handle connection to the individual ESP32 Cameras."""

    def __init__(
        self,
        num=1,
        base_ip="192.168.4",
        image_dir="test/images",
        admin_pass="change-me-ota",
    ):
        """Initialise the Camera object."""
        self.number = num
        ip = ".".join([base_ip, str(self.number + 10)])
        self.request_handle = RequestHandler(ip, admin_pass)
        self._shot_status = {"state": "disconnected"}
        self._idle_s = -1
        self.exposure_locked = False
        self.last_status_update = 0
        self.update_interval = 1
        self.image_dir = image_dir

    def cleanup(self):
        """Disarm the cameras, cleanup the request handler."""
        # self.disarm()
        self.request_handle.cleanup()

    @property
    def shot_status(self):
        """Shot Status Dictionary, read from the camera."""
        now = time.time()
        try:
            if now - self.last_status_update > self.update_interval:
                self.last_status_update = now
                # timeout of half a second. Should not take even that long.
                response = self.request_handle.get("shot_status", 0.5)
                self._shot_status = {
                    k: v for k, v in response.items() if k in STATUS_KEYS
                }

                self.exposure_locked = response.get("locked", False)
        except CameraError:
            # assume an error here means the camera isn't connected to the wifi
            self._shot_status = {"state": "disconnected"}
            self.exposure_locked = False

        return self._shot_status

    @property
    def idle_s(self):
        """Number of seconds the camera is idle before automatically disarming."""
        if self._idle_s < 0:
            try:
                response = self.request_handle.get("config", 0.5)
                self._idle_s = response.get("idle_s", -1)
            except CameraError:
                self._idle_s = -1
        return self._idle_s

    def arm(self, arm: bool):
        """Arm or disarm the Camera."""
        if arm:
            response = self.request_handle.post("arm", None)
        else:
            response = self.request_handle.post("disarm", None)
        self._shot_status = response
        self.last_status_update = time.time()

    def lock_exposure(self, lock: bool):
        """Lock or unlock the exposure, white balance, and gain settings."""
        if lock:
            response = self.request_handle.post("lock", None)
        else:
            response = self.request_handle.post("unlock", None)
        self.exposure_locked = response.get("locked", False)

        self._shot_status = {k: v for k, v in response.items() if k in STATUS_KEYS}
        self.last_status_update = time.time()

    def trigger(self):
        """Trigger the camera to take a picture using the software trigger."""
        logging.debug(f"Camera {self.number} triggererd to take an image")
        response = self.request_handle.post("trigger", None)
        self._shot_status = {k: v for k, v in response.items() if k in STATUS_KEYS}
        self.last_status_update = time.time()

    def get_image(self):
        """Get the image from the buffer."""
        response: bytes = self.request_handle.get("shot", timeout=10, type="image")
        return response

    def set_config(self, config: dict[str, Any]):
        """Set the config of the firmware. This will cause the ESP32 to reboot."""
        self.request_handle.patch("config", config)
        # Set idle_s to negative so it has to get it from config again
        self._idle_s = -1


class RequestHandler:
    """Request handling class, to simplify the GET/PUT/POST requests to a camerea API."""

    def __init__(self, ip: str, admin_pass: str):
        """Create the request session for this IP Addr."""
        self.addr = ip_address(ip)
        self.session = requests.Session()
        self.session.headers.update({"X-Auth": admin_pass})

    def get(self, path: str, timeout=5, type="json") -> dict[str, Any] | bytes:
        """Do a get request, returning the json dict of the response."""
        try:
            fullpath = f"http://{osPath.join(self.addr.compressed, path)}"
            logging.debug(f"GET request at addr {fullpath}")
            response = self.session.get(fullpath, timeout=timeout)
            response.raise_for_status()
            match type:
                case "json":
                    return response.json()
                case "image" | "bytes" | _:
                    return response.content

        except requests.exceptions.RequestException as error:
            logging.error("Error Occured during GET Request")
            # Map the various requests exception types to the appropriate status code
            if isinstance(error, requests.exceptions.ConnectionError):
                logging.error("Connection Error")
                raise CameraError(
                    f"Connection Error for addr {fullpath}. Check power to device"
                )
            elif isinstance(error, requests.exceptions.Timeout):
                logging.error("Timeout Error")
                raise CameraError(
                    f"Timeout trying to GET from {fullpath}, timeout: {timeout}"
                )
            elif isinstance(error.response, requests.Response):
                err_response: dict = error.response.json()
                if "error" in err_response.keys():
                    err_response = err_response["error"]
            else:
                err_response = error
            raise CameraError(f"Unknown Error: {err_response}")

    def post(self, path: str, data, timeout=5) -> dict[str, Any]:
        """Do a Put request with the provided data."""
        try:
            fullpath = f"http://{osPath.join(self.addr.compressed, path)}"
            logging.debug(f"POST request at addr {fullpath}")
            response = self.session.post(fullpath, data, timeout=timeout)

            response.raise_for_status()

            return response.json()

        except requests.exceptions.RequestException as error:
            logging.error("Error during PUT request")
            if isinstance(error, requests.exceptions.ConnectionError):
                logging.error("Connection Error")
                raise CameraError(
                    f"Connection Error for addr {fullpath}. Check power to device"
                )
            elif isinstance(error, requests.exceptions.Timeout):
                logging.error("Timeout Error")
                raise CameraError(
                    f"Timeout trying to POST to {fullpath}, timeout: {timeout}"
                )
            elif isinstance(error.response, requests.Response):
                err_response: dict = error.response.json()
                if "error" in err_response.keys():
                    err_response = err_response["error"]
            else:
                err_response = error
            raise CameraError(f"Unknown Error: {err_response}")

    def patch(self, path: str, data: dict[str, Any], timeout=5):
        """Do a Patch request with the provided data. Translate the dict to the urlencoded body for the patch."""
        try:
            fullpath = f"http://{osPath.join(self.addr.compressed, path)}"
            logging.debug(f"PATCH request at addr {fullpath}")
            response = self.session.patch(fullpath, data, timeout=timeout)
            response.raise_for_status()

            return response.json()

        except requests.exceptions.RequestException as error:
            logging.error("Error during PATCH request")
            if isinstance(error, requests.exceptions.ConnectionError):
                logging.error("Connection Error")
                raise CameraError(
                    f"Connection Error for addr {fullpath}. Check power to device"
                )
            elif isinstance(error, requests.exceptions.Timeout):
                logging.error("Timeout Error")
                raise CameraError(
                    f"Timeout trying to PATCH to {fullpath}, timeout: {timeout}"
                )
            elif isinstance(error.response, requests.Response):
                err_response: dict = error.response.json()
                if "error" in err_response.keys():
                    err_response = err_response["error"]
            else:
                err_response = error
            raise CameraError(f"Unknown Error: {err_response}")

    def cleanup(self):
        """Shutdown HTTP session."""
        self.session.close()
        pass
