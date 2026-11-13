complete -c wayviewer --no-files

complete -c wayviewer      -l port -d 'TCP port to listen on (default: 1234)' --exclusive
complete -c wayviewer      -l host -d 'Address to bind (default: 0.0.0.0)' --exclusive
complete -c wayviewer -s h -l help -d 'Prints help'
