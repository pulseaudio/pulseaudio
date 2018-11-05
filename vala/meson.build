vala_apis = [
  'libpulse',
  'libpulse-mainloop-glib',
  'libpulse-simple',
]

foreach api : vala_apis
  install_data(api + '.deps', api + '.vapi',
    install_dir : vapidir)
endforeach
