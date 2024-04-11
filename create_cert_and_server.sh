#!/bash/bin

# workspace path
workspace=$( pwd )

# set desired project path
path="/home/jang175/esp/esp-projects/RGB_blink"

# go to desired project path
cd "$path/build"

# check if cert and key already exist
if test -e "ca_cert.pem" && test -e "ca_key.pem"; then
    echo "Cert and key already exist"
else
    echo "Create cert and key"
    openssl req -x509 -newkey rsa:2048 -keyout ca_key.pem -out ca_cert.pem -days 365 -nodes

    clear
fi

# copy cert and key to server_certs in workspace
cp ca_cert.pem "$workspace/server_certs/"

# start server
echo "Starting server..."
openssl s_server -WWW -key ca_key.pem -cert ca_cert.pem -port 8070