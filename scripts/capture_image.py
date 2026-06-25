import requests
import datetime
import os
# includes for gdrive sync
from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError
from apiclient.http import MediaFileUpload,MediaIoBaseDownload

# If modifying these scopes, delete the file token.json.
SCOPES = ["https://www.googleapis.com/auth/drive.file"]

def upload_to_gdrive(filepath):
  """Use Drive v3 API to upload to gdrive.
  """
  creds = None
  # The file token.json stores the user's access and refresh tokens, and is
  # created automatically when the authorization flow completes for the first
  # time.
  if os.path.exists("token.json"):
    creds = Credentials.from_authorized_user_file("token.json", SCOPES)
  # If there are no (valid) credentials available, let the user log in.
  if not creds or not creds.valid:
    if creds and creds.expired and creds.refresh_token:
      creds.refresh(Request())
    else:
      flow = InstalledAppFlow.from_client_secrets_file(
          "credentials.json", SCOPES
      )
      creds = flow.run_local_server(port=0)
    # Save the credentials for the next run
    with open("token.json", "w") as token:
      token.write(creds.to_json())

  try:
    service = build("drive", "v3", credentials=creds)

    file_metadata = {
    'name': filepath,
    'mimeType': 'image/png'
    }
    media = MediaFileUpload(filepath,
                            mimetype='*/*',
                            resumable=True)
    file = service.files().create(body=file_metadata, media_body=media, fields='id').execute()
    print ('File ID: ' + file.get('id'))
  except HttpError as error:
    # TODO(developer) - Handle errors from drive API.
    print(f"An error occurred: {error}")

def capture_image_and_sync(capture_url, sensor_url):
    """
    Sends a GET request to the 'readSensor' API endpoint and verifies the response.
    If successful, invoke 'capture' API and append sensor data.

    Args:
        capture_url (str): The URL of the 'capture' API endpoint.
                  Example: "http://192.168.0.102/capture"

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
            # Upload file to gdrive
            try:
                with open(filename, 'wb') as f:
                    f.write(response.content)
                    f.close()
                upload_to_gdrive(filename)
            except Exception as e:
                print(f"Error: Exception caught !!: {e}")
        else:
            # Handle non-200 status codes.  Print the status code and text.
            print(f"Error: API request failed with status code {response.status_code}")
            print("Response text:", response.text)
    except requests.exceptions.RequestException as e:
        # Handle network errors (e.g., connection refused, timeout).
        print(f"Error: An error occurred during the request: {e}")

if __name__ == "__main__":
    # Invoke API endpoint.
    capture_url = "http://192.168.0.101/capture"
    sensor_url  = "http://192.168.0.101/readSensor"

    # Capture image and append sensor data
    capture_image_and_sync(capture_url, sensor_url)

