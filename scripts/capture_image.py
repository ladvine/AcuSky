import requests
import datetime

def capture_image_and_sync(capture_url, sensor_url):
    """
    Sends a GET request to the 'readSensor' API endpoint and verifies the response.
    If successful, invoke 'capture' API and append sensor data.

    Args:
        capture_url (str): The URL of the 'capture' API endpoint.
                  Example: "http://192.168.0.103/capture"

    Returns:
        Saves image with the sensor data and timestamp in filename.
        Handles potential errors and edge cases.
    """
    try:
        # Get the current date and time.
        now = datetime.datetime.now()
        timestamp_str = now.strftime("%Y%m%d#%H%M%S")

        # Send a GET request to the sensor API endpoint.
        response = requests.get(sensor_url)

        # Construct the filename.
        filename = f"IMG_{timestamp_str}_{response.text}.png"

        # Send a GET request to the capture API endpoint.
        response = requests.get(capture_url)

        # Print the full URL for debugging
        print(f"Request URL: {response.url}")

        # Check the response status code.  200 indicates success.
        if response.status_code == 200:
            # Attempt to parse the JSON response.
            try:
                with open(filename, 'wb') as f:
                    f.write(response.content)

            except:
                print("Error: Exception caught !!")
                print("Response text:", response.text)  # Print the raw text
        else:
            # Handle non-200 status codes.  Print the status code and text.
            print(f"Error: API request failed with status code {response.status_code}")
            print("Response text:", response.text)
    except requests.exceptions.RequestException as e:
        # Handle network errors (e.g., connection refused, timeout).
        print(f"Error: An error occurred during the request: {e}")

if __name__ == "__main__":
    # Invoke API endpoint.
    capture_url = "http://192.168.0.103/capture"
    sensor_url  = "http://192.168.0.103/readSensor"

    # Capture image and append sensor data
    capture_image_and_sync(capture_url, sensor_url)

