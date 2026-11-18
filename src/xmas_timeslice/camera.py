"""Module for the Camera Class, which connects to a single camera via HTTP for config and image copying."""

import logging
import time
from ipaddress import ip_address
from os import listdir, path
from tornado.ioloop import IOLoop

import requests


class CameraError(Exception):
    """Exception class for Camera errors."""

    pass


STATUS_KEYS = [
    "state",
    "ip",
    "error",
    "locked",
    "missed",
    "shot_id",
    "size",
    "width",
    "height",
]


class Camera:
    """Camera Class, to handle connection to the individual ESP32 Cameras."""

    def __init__(self, num=1, base_ip="192.168.4", image_dir="test/images"):
        """Initialise the Camera object."""
        self.number = num
        ip = ".".join([base_ip, str(self.number + 10)])
        self.request_handle = RequestHandler(ip)
        self._shot_status = {"state": "disconnected"}
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
                response = self.request_handle.get("shot_status", 0.2) # timeout of half a second. Should not take even that long.
                self._shot_status = {k: v for k, v in response.items() if k in STATUS_KEYS}
        except CameraError:
            pass
        finally:
            return self._shot_status

    def arm(self, arm: bool):
        """Arm or disarm the Camera."""
        # TODO: I don't like the fact that arming/disarming/triggering is done with a GET request. Should be a PUT
        if arm:
            response = self.request_handle.get("arm")
        else:
            response = self.request_handle.get("disarm")
        return response

    def lock_exposure(self, lock: bool):
        """Lock or unlock the exposure, white balance, and gain settings."""
        if lock:
            response = self.request_handle.get("lock")
        else:
            response = self.request_handle.get("unlock")
        return response

    def trigger(self):
        """Trigger the camera to take a picture using the software trigger."""
        logging.debug(f"Camera {self.number} triggererd to take an image")
        response = self.request_handle.get("trigger")
        return response

    def get_image(self):
        """Get the image from the buffer."""
        response: bytes = self.request_handle.get("shot", timeout=10, type="image")
        return response




class RequestHandler:
    """Request handling class, to simplify the GET/PUT/POST requests to a camerea API."""

    def __init__(self, ip: str):
        """Create the request session for this IP Addr."""
        self.addr = ip_address(ip)
        self.session = requests.Session()

    def get(self, path: str, timeout=5, type="json") -> dict[str] | bytes:
        """Do a get request, returning the json dict of the response."""
        try:
            fullpath = "/".join(["http:/", self.addr.compressed, path])
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
                raise CameraError(
                    f"Connection Error for addr {fullpath}. Check power to device"
                )
            elif isinstance(error, requests.exceptions.Timeout):
                raise CameraError(f"Timeout trying to GET from {fullpath}, timeout: {timeout}")
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
